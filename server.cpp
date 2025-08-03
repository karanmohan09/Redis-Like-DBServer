#include <iostream>
#include <cstring>
#include <cassert>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
//c++ libs
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/epoll.h>
//networking headers
#include "hashtable.cpp"
//Main Key Value store Logic

static std:: ofstream aof_file;
static bool aof_loading = false;
static int aof_cmd_count = 0;
static int AOF_COMPACT_THRESHOLD = 1000;
const size_t k_max_msg = 32 << 20;

#define container_of(ptr, type, member)({\
    const typeof(((type *)0)->member)*__mptr = (ptr);\
    (type * )((char *)__mptr - offsetof(type, member));})


enum
{
    STATE_REQ = 0, //read request
    STATE_RES = 1, //send responses
    STATE_END = 2, //mark connection for deletion
};

enum
{
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

enum
{
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_ARR = 4,
};

enum
{
    ERR_2BIG = 2,
    ERR_UNKNOWN = 1,
};

static struct 
{
    HMap db;
}g_data;

struct Conn
{
    int fd = -1;
    uint32_t state = 0;//read request or send response state

    size_t read_buffer_size = 0;
    uint8_t read_buffer[4 + k_max_msg];
    size_t read_buffer_consumed = 0;

    size_t write_buffer_size = 0;
    size_t write_buffer_sent = 0;
    uint8_t write_buffer[4 + k_max_msg];    
};

static void msg(const char * msg)
{
    std:: cout << msg << std:: endl;
}

static void die(const char * msg)
{
    int error_code = errno;
    fprintf(stderr, "[%d] %s", error_code, msg);
    abort();
}

static uint64_t str_hash(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811C9DC5;
    for(size_t i = 0 ; i < len ; i++)
    {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

static void out_nil(std:: string &out)
{
    out.push_back(SER_NIL);
}

static void out_str(std:: string &out, const std:: string &val)
{
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)val.size();
    out.append((char *)&len, 4);
    out.append(val);
}

static void out_int(std:: string &out, int64_t val)
{
    out.push_back(SER_INT);
    out.append((char *)&val, 8);
}

static void out_err(std:: string &out, int32_t code, const std:: string &msg)
{
    out.push_back(SER_ERR);
    out.append((char *)&code, 4);
    uint32_t len = (uint32_t)msg.size();
    out.append((char *)&len, 4);
    out.append(msg);
}

static void out_arr(std:: string &out, uint32_t n)
{
    out.push_back(SER_ARR);
    out.append((char *)&n, 4);
}

static void h_scan(HTab *tab, void (*f)(HNode *, void *), void *arg)
{
    if(tab->size == 0)
    {
        return;
    }
    for(size_t i = 0 ; i < tab->mask + 1; i++)
    {
        HNode * node = tab->tab[i];
        while(node)
        {
            f(node, arg);
            node = node->next;
        }
    }
}

static void maybe_compact_aof();

static void append_to_aof(const std:: vector<std:: string>& cmd)
{
    if(aof_loading) return;
    uint32_t n = (uint32_t)cmd.size();
    aof_file.write((char *)&n, 4);
    for(const auto &s : cmd)
    {
        uint32_t len = (uint32_t)s.size();
        aof_file.write((char *)&len, 4);
        aof_file.write(s.data(), len);
    }
    aof_file.flush();
    ++aof_cmd_count;
    if(aof_cmd_count >= AOF_COMPACT_THRESHOLD)
    {
        maybe_compact_aof();
        aof_cmd_count = 0;
    }
}

static void cb_scan(HNode *node, void *arg)
{
    std:: string &out = *(std:: string *)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static void do_keys(std:: vector<std:: string>&cmd, std:: string &out)
{
    (void)cmd;
    out_arr(out, (uint32_t)hm_size (&g_data.db));
    h_scan(&g_data.db.h1, &cb_scan, &out);
    h_scan(&g_data.db.h2, &cb_scan, &out);
}

static bool entry_eq(HNode *lhs, HNode *rhs)
{
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}


static void do_del(std:: vector<std:: string>&cmd, std:: string &out)
{
    Entry key;
    key.key = cmd[1];
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode * node = hm_pop(&g_data.db, &key.node, &entry_eq);
    if(node)
    {
        delete container_of(node, Entry, node);
        append_to_aof(cmd);
    }
    return out_int(out, node ? 1 : 0);
}

static void fd_set_nb(int conn_fd)
{
    errno = 0;
    int flags = fcntl(conn_fd, F_GETFL, 0);
    if(errno)
    {
        die("fcntl error.");
        return;
    }

    flags |= O_NONBLOCK;
    errno = 0;

    (void)fcntl(conn_fd, F_SETFL, flags);
    if(errno)
    {   
        die("fcntl error.");
    }
}

static void conn_put(std:: vector<Conn * >&fd2conn, struct Conn * conn)
{
    if(fd2conn.size() <= (size_t)conn->fd)
    {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std:: vector<Conn *> &fd2conn, int fd) // create new struct conn object by accepting new connection 
{
    //accept a new connection
    struct sockaddr_in client_address = {};
    socklen_t socklen = sizeof(client_address);
    int conn_fd = accept(fd, (struct sockaddr *)&client_address, &socklen);
    if(conn_fd < 0)
    {
        msg("accept() error");
        return -1;
    }

    //set new conn_fd to non blockng mode
    fd_set_nb(conn_fd);
    struct Conn *conn = (struct Conn *)calloc(1, sizeof(struct Conn));
    if(!conn)
    {
        close(conn_fd);
        return -1;
    }

    conn->fd = conn_fd;
    conn->state = STATE_REQ;
    conn->read_buffer_size = 0;
    conn->write_buffer_size = 0;
    conn->write_buffer_sent = 0;
    conn_put(fd2conn, conn);
    return conn_fd;
}

static bool try_flush_buffer(Conn * conn)
{
    ssize_t rv = 0;
    size_t remain = conn->write_buffer_size - conn->write_buffer_sent;
    rv = write(conn->fd, &conn->write_buffer[conn->write_buffer_sent], remain);
    if(rv < 0 && errno == EAGAIN)
    {
        return false;
    }
    if(rv < 0)
    {
        msg("write: error.");
        conn->state = STATE_END;
        return false;
    }
    conn->write_buffer_sent += (size_t)rv;
    assert(conn->write_buffer_sent <= conn->write_buffer_size);
    if(conn->write_buffer_sent == conn->write_buffer_size)//responsefully sent
    {
        conn->state = STATE_REQ;
        conn->write_buffer_sent = 0;
        conn->write_buffer_size = 0;
        return false;
    }
    return true;    
}

static void state_res(Conn * conn)
{
    while(try_flush_buffer(conn)){}
}

static int32_t parse_request(const uint8_t *request_data, size_t request_len, std::vector<std:: string> &cmd)
{
    if(request_len < 4)
    {
        return -1;
    }

    uint32_t n_strings = 0;
    memcpy(&n_strings, &request_data[0], 4);
    if(n_strings > k_max_msg)
    {
        return -1;
    }

    size_t pos = 4;//byte offset into the databuffer
    while(n_strings--)
    {
        if(pos + 4 > request_len)
        {
            return -1;
        }
        uint32_t argument_size = 0;
        memcpy(&argument_size, &request_data[pos], 4);
        if(pos + 4 + argument_size > request_len)
        {
            return -1;
        }
        cmd.push_back(std::string((char *) &request_data[pos + 4], argument_size));
        pos += 4 + argument_size;
    }

    if(pos != request_len)
    {
        return -1; //some trailing garbage data
    }
    return 0;
}

static void do_get(const std:: vector<std:: string> &cmd, std:: string &out)
{
    Entry key;
    key.key = cmd[1];
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode * node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(!node)
    {
        return out_nil(out);
    }
    std:: string & val = container_of(node, Entry, node)->val;
    return out_str(out, val);
}

static void do_set(const std:: vector<std:: string> &cmd, std:: string &out)
{
    Entry key;
    key.key = cmd[1];
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode * node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(node)
    {
        container_of(node, Entry, node)->val = cmd[2];
    }
    else
    {
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val = cmd[2];
        hm_insert(&g_data.db, &ent->node);
    }
    append_to_aof(cmd);
    return out_nil(out);
}
void do_request(std:: vector<std:: string>&cmd, std:: string &out)
{
    if(cmd.size() == 1 && cmd[0] == "keys")
    {
        do_keys(cmd, out);
    }
    else if(cmd.size() == 2 && cmd[0] == "get")
    {
        do_get(cmd, out);
    }
    else if(cmd.size() == 3 && cmd[0] == "set")
    {
        do_set(cmd, out);
    }
    else if(cmd.size() == 2 && cmd[0] == "del")
    {
        do_del(cmd, out);
    }
    else if(cmd.size() == 1 && cmd[0] == "clear")
    {
        hm_clear(&g_data.db);
        if(!aof_loading)
        {
            append_to_aof(cmd);
        }
        out_nil(out);
    }
    else
    {
        out_err(out, ERR_UNKNOWN, "unknown command");
    }
    
}


static bool try_one_request(Conn * conn)
{
    size_t available = conn->read_buffer_size - conn->read_buffer_consumed;
    if(available < 4) return false;

    uint32_t request_len = 0;
    memcpy(&request_len, conn->read_buffer + conn->read_buffer_consumed, 4);
    if(4 + request_len > available) return false;
    //
    //
    const uint8_t * request_data = conn->read_buffer + conn->read_buffer_consumed + 4;
    std:: vector<std:: string>cmd;
    if(0 != parse_request(request_data, request_len, cmd))
    {
        msg("Bad Request");
        conn->state = STATE_END;
        return false;
    }

    std:: string out;
    do_request(cmd, out);
    if(4 + out.size() > k_max_msg)
    {
        out.clear();
        out_err(out, ERR_2BIG, "Msg is too big.");
    }
    
    uint32_t response_len = (uint32_t)out.size();
    memcpy(&conn->write_buffer[0], &response_len, 4);
    memcpy(&conn->write_buffer[4], out.data(), out.size());
    conn->write_buffer_size = 4 + response_len;

    conn->read_buffer_consumed += 4 + request_len;
    conn->state = STATE_RES;
    state_res(conn);
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn * conn)
{
    if(conn->read_buffer_consumed > 0)
    {
        if(conn->read_buffer_size == sizeof(conn->read_buffer))
        {
            size_t remain = conn->read_buffer_size - conn->read_buffer_consumed;
            memmove(conn->read_buffer, conn->read_buffer + conn->read_buffer_consumed, remain);
            conn->read_buffer_size = remain;
            conn->read_buffer_consumed = 0;
        }
    }
    assert(conn->read_buffer_size < sizeof(conn->read_buffer));
    ssize_t rv = 0;
    do
    {
        ssize_t cap = sizeof(conn->read_buffer) - conn->read_buffer_size;
        rv = read(conn->fd, &conn->read_buffer[conn->read_buffer_size], cap);
        
    } 
    while (rv < 0 && errno == EINTR);
    if(rv < 0 && errno == EAGAIN)
    {
        return false;
    }
    if(rv < 0)
    {
        msg("Read Error");
        conn->state = STATE_END;
        return false;
    }

    if(rv == 0)
    {
        msg("EOF");
        conn->state = STATE_END;
        return false;
    }
    conn->read_buffer_size += (ssize_t)rv;
    assert(conn->read_buffer_size <= sizeof(conn->read_buffer));

    while(try_one_request(conn)){};
    return (conn->state == STATE_REQ);
}

static void state_req(Conn * conn)//for reading
{
    while(try_fill_buffer(conn)) {}
}

static void connection_io(Conn * conn)//state machine for our client connections
{
    if(conn->state == STATE_REQ)
    {
        state_req(conn);
    }
    else if(conn->state == STATE_RES)
    {
        state_res(conn);
    }
    else
    {
        assert(0);
    }
}

static void load_aof(const std:: string& filename)
{   
    std:: ifstream in(filename, std:: ios:: binary);
    if(!in.is_open()) return;
    aof_loading = true;
    uint32_t replayed_write_code = 0;
    while(true)
    {
        uint32_t n = 0;
        if(!in.read((char *)&n, 4)) break;
        std:: vector<std:: string> cmd;
        for(uint32_t i = 0 ; i < n ; i++)
        {
            uint32_t len = 0;
            if(!in.read((char *)&len, 4)) return;
            std:: string s(len, '\0');
            if(!in.read(&s[0], len)) return;
            cmd.push_back(std:: move(s));
        }
        std:: string discard_out;
        do_request(cmd, discard_out);

        if(!cmd.empty())
        {
            const std:: string &op = cmd[0];
            if(op == "set" || op == "del")
            {
                ++replayed_write_code;
            }
            else if(op == "clear")
            {
                hm_clear(&g_data.db);
            }
        }
    }
    aof_loading = false;
    in.close();
    aof_cmd_count = replayed_write_code % AOF_COMPACT_THRESHOLD;
}

static void maybe_compact_aof()
{
    if(aof_loading) return;

    std:: string temp_file_name = "appendonly.aof.temp";
    std:: ofstream temp_aof(temp_file_name, std:: ios:: binary | std:: ios:: trunc);
    if(!temp_aof.is_open())
    {
        msg("Failed to open temp AOF file for compaction");
        return;
    }

    auto dump_entry = [](HNode * node, void * arg)
    {
        std:: ofstream *out = (std:: ofstream *)arg;
        Entry *e = container_of(node, Entry, node);
        std:: vector<std:: string> cmd = {"set", e->key, e->val};
        uint32_t n = (uint32_t)cmd.size();
        out->write((char *)&n, 4);
        for(const auto &s : cmd)
        {
            uint32_t len = (uint32_t)s.size();
            out->write((char*)&len, 4);
            out->write(s.data(), len);
        }
    };

    h_scan(&g_data.db.h1, dump_entry, &temp_aof);
    h_scan(&g_data.db.h2, dump_entry, &temp_aof);
    temp_aof.close();

    aof_file.close();
    if(rename(temp_file_name.c_str(), "appendonly.aof") != 0)
    {
        msg("Failed to rename Temp AOF file.");
        aof_file.open("appendonly.aof", std:: ios:: app | std:: ios:: out);
        return;
    }
    aof_file.open("appendonly.aof", std:: ios:: app | std:: ios:: out);
    if(!aof_file.is_open())
    {
        die("Failed to reopen AOF file after completion");
    } 
    msg("AOF compaction complete");
}

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0)
    {
        die("socket() : Error");
    } 
    int val = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));    
    
    struct sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = ntohs(1234);
    server_address.sin_addr.s_addr = ntohl(0);//0.0.0.0/1234
    int rv = bind(server_fd, (const sockaddr *)& server_address, sizeof(server_address));
    if(rv)
    {
        die("bind(): error");
    }
    rv = listen(server_fd, SOMAXCONN);
    if(rv)
    {
        die("listen(): error");
    }

    std:: vector<Conn *> fd2conn;//map of all clients keyd by their conn_fds
    fd_set_nb(server_fd);//set the serverfd to be non blocking

    //
    int epoll_fd = epoll_create1(0);
    if(epoll_fd < 0)
    {
        die("epoll create1()");
    }

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
    {
        die("epoll_ctl : server_fd");
    }
    std:: vector<struct epoll_event>epoll_event(1024);
    //
    aof_file.open("appendonly.aof", std:: ios:: out | std:: ios:: app);
    if(!aof_file.is_open())
    {
        die("Failed to open AOF file");
    }
    load_aof("appendonly.aof");
    while(true)
    {
        int n = epoll_wait(epoll_fd, epoll_event.data(), (int)epoll_event.size(), -1);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            die("epoll_wait()");
        }
        //connections fds
        for(int i = 0 ; i < n ; ++i)
        {
            int fd = epoll_event[i].data.fd;

            if(fd == server_fd)
            {
                int conn_fd = accept_new_conn(fd2conn, server_fd);
                if(conn_fd >=0)
                {
                    struct epoll_event conn_ev = {};
                    conn_ev.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLOUT;
                    conn_ev.data.fd = conn_fd;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &conn_ev) < 0)
                    {
                        die("epoll_ctl: conn_fd");
                    }
                }
            }
            else
            {
                Conn *conn = fd2conn[fd];
                if(!conn) continue;

                connection_io(conn);
                if(conn->state == STATE_END)
                {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
                    close(conn->fd);
                    free(conn);
                    fd2conn[fd] = nullptr;
                }
                else
                {
                    struct epoll_event conn_ev = {};
                    conn_ev.data.fd = conn->fd;
                    conn_ev.events = (conn->state == STATE_REQ ? EPOLLIN : EPOLLOUT) | EPOLLET | EPOLLERR;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &conn_ev) < 0)
                    {
                        die("epoll_ctl : modify conn_fd");
                    }
                }
            }
        }
    }
    aof_file.close();
    return 0;
}