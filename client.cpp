#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <string.h>
#include <vector>
const size_t k_max_msg = 4096;
enum
{
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_ARR = 4,
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

static int32_t read_full(int conn_fd, char* read_buffer, size_t n)
{
    while(n > 0)
    {
        ssize_t rv = read(conn_fd, read_buffer, n);
        if(rv <= 0)
        {
            return -1;
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        read_buffer += rv;
    }
    return 0;
}

static int32_t write_full(int conn_fd, char* write_buffer, size_t n)
{
    while(n > 0)
    {
        ssize_t rv = write(conn_fd, write_buffer, n);
        if(rv <= 0)
        {
            return -1;//-1 unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= rv;
        write_buffer += rv;
    }
    return 0;
}

static int32_t send_req(int conn_fd, const std::vector<std:: string>& cmd)
{
    uint32_t len = 4;
    for(const std:: string &s : cmd)
    {
        len += 4 + s.size();
    }

    if(len > k_max_msg)
    {
        return -1;
    }

    char write_buffer[4 + k_max_msg];
    memcpy(write_buffer, &len, 4);
    int n = cmd.size();
    memcpy(&write_buffer[4], &n, 4);
    size_t cur = 8;
    for(const std:: string &s : cmd)
    {
        uint32_t p = (uint32_t)s.size();
        memcpy(&write_buffer[cur], &p, 4);
        memcpy(&write_buffer[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_full(conn_fd, write_buffer, 4 + len);
}

static int32_t on_response(const uint8_t *data, size_t size)
{
    if(size < 1)
    {
        msg("Bad response.");
        return -1;
    }

    switch(data[0])
    {
        case SER_NIL:
            printf("(nil)\n");
            return 1;
        case SER_ERR:
            if(size < 1 + 8)
            {
                msg("Bad response.");
                return -1;
            }
            else
            {
                int32_t code = 0;
                uint32_t len = 0;
                memcpy(&code, &data[1], 4);
                memcpy(&len, &data[1 + 4], 4);
                if(size < 1 + 8 + len)
                {
                    msg("Bad response");
                    return -1;
                }
                printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
                return 1 + 8 + len;
            }
        case SER_STR:
            if(size < 1 + 4)
            {
                msg("Bad response");
                return -1;
            }
            else
            {
                uint32_t len = 0;
                memcpy(&len, &data[1], 4);
                if(size < 1 + 4 + len)
                {
                    msg("Bad response.");
                    return -1;
                }
                printf("(str) %.*s\n", len, &data[1 + 4]);
                return 1 + 4 + len;
            }
        case SER_INT:
            if(size < 1 + 8)
            {
                msg("Bad response");
                return -1;
            }
            else
            {
                int64_t val = 0;
                memcpy(&val, &data[1], 8);
                printf("(int) %ld\n", val);
                return 1 + 8;
            }
        case SER_ARR:
            if(size < 1 + 4)
            {
                msg("Bad response");
                return -1;
            }
            else
            {
                uint32_t len = 0;
                memcpy(&len, &data[1], 4);
                printf("(arr) len = %u\n", len);
                size_t arr_bytes = 1 + 4;
                for(uint32_t i = 0 ; i  < len ; i++)
                {
                    int32_t rv = on_response(&data[arr_bytes], size - arr_bytes);
                    if(rv < 0)
                    {
                        return rv;
                    }
                    arr_bytes += (size_t)rv;
                }
                printf("(arr) end\n");
                return (int32_t) arr_bytes;
            }
        default:    
            msg("Bad Response");
            return -1;
    }
}

static int32_t read_res(int conn_fd)
{
    char read_buffer[4 + k_max_msg + 1];
    errno = 0;
    int32_t err = read_full(conn_fd, read_buffer, 4);
    if(err)
    {
        if(errno == 0)
        {
            msg("EOF Error");
        }
        else
        {
            msg("read error.");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, read_buffer, 4);
    if(len > k_max_msg)
    {
        msg("Msg too long.");
        return -1;
    }

    err = read_full(conn_fd, &read_buffer[4], len);
    if(err)
    {
        msg("Read Error.");
        return err;
    }

    int32_t rv = on_response((uint8_t *)&read_buffer[4], len);
    return 0;
}

int main(int argc, char ** argv)
{
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(client_fd < 0)
    {
        msg("Socket Failure, Exiting..");
        return -1;
    }

    struct sockaddr_in client_address = {};
    client_address.sin_family = AF_INET;
    client_address.sin_port = ntohs(1234);
    client_address.sin_addr.s_addr = ntohl(INADDR_LOOPBACK);
    int rv = connect(client_fd, (const struct sockaddr *)&client_address, sizeof(client_address));

    if(rv < 0)
    {
        msg("Couldnt Connect.\n");
        return -1;
    }

    std::vector<std:: string> cmd;
    for(int i = 1 ; i < argc ; ++i)
    {
        cmd.push_back(argv[i]);
    }
    u_int32_t error_code = send_req(client_fd, cmd);
    if(error_code)
    {
        goto L_DONE;
    }
    error_code = read_res(client_fd);
    if(error_code)
    {
        goto L_DONE;
    }
L_DONE:
    close(client_fd);
    return 0;
}