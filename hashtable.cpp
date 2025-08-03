#include<iostream>
#include <cassert>
const size_t k_resizing_work = 128;
const size_t k_max_load_factor = 8;

//n should be power of 2

struct HNode
{
    u_int64_t hcode = 0;
    HNode * next = nullptr;
};

struct HTab
{
    HNode ** tab = nullptr;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap
{
    HTab h1;
    HTab h2;
    size_t resizing_pos = 0;
};

struct Entry
{
    HNode node;
    std:: string key;
    std:: string val;
};

#define container_of(ptr, type, member)({\
    const typeof(((type *)0)->member)*__mptr = (ptr);\
    (type * )((char *)__mptr - offsetof(type, member));})


static void hm_clear(HMap *map)
{
    for(int phase = 0 ; phase < 2 ; ++phase)
    {
        HTab *ht = (phase == 0)? &map->h1 : &map->h2;
        if(!ht->tab) continue;
        for(size_t i = 0 ; i <= ht->mask ; ++i)
        {
            HNode * node = ht->tab[i];
            while(node)
            {
                HNode * next = node->next;
                Entry* ent = container_of(node, Entry, node);
                delete ent;
                node = next;
            }
            ht->tab[i] = nullptr;
        }
        free(ht->tab);
        ht->tab = nullptr;
        ht->mask = 0;
        ht->size = 0;
    }
    map->resizing_pos = 0;
}

static void h_init(HTab *htab, size_t n)
{
    assert(n > 0 && ((n - 1)&n) == 0);
    htab->tab = (HNode **)calloc(sizeof(HNode *) ,n);
    htab->mask = n - 1;
    htab->size = 0; 
}

static void h_insert(HTab *htab, HNode * hnode)
{
    size_t pos = hnode->hcode & htab->mask;
    HNode * next = htab->tab[pos];
    hnode->next = next;
    htab->tab[pos] = hnode;
    htab->size++;
}

static HNode **h_lookup (HTab *htab, HNode * keynode, bool(*cmp)(HNode *, HNode *))
{
    if(!htab->tab)
    {
        return nullptr;
    }
    size_t pos = htab->mask & keynode->hcode;
    HNode ** from  = &htab->tab[pos];
    while(*from)
    {
        if(cmp(*from, keynode))
        {
            return from;
        }
        from = &(*from)->next;
    }
    return nullptr;
}

struct HNode * h_detach(HTab *htab, HNode **from)
{
    HNode * node = *from;
    *from = (*from)->next;
    htab->size--;
    return node;
}

static void hm_help_resizing(HMap * hmap)
{
    if(hmap->h2.tab == nullptr)
    {
        return;
    }

    size_t n_work = 0;
    while(n_work < k_resizing_work && hmap->h2.size > 0)
    {
        HNode **from = &hmap->h2.tab[hmap->resizing_pos];
        if(!*from)
        {
            hmap->resizing_pos++;
            continue;
        }
        h_insert(&hmap->h2, h_detach(&hmap->h2, from));
        n_work++;
    }

    if(hmap->h2.size == 0)
    {
        free(hmap->h2.tab);
        hmap->h2 = HTab{};
    }
}

HNode * hm_lookup(HMap * hmap, HNode * key_node, bool(*cmp)(HNode *, HNode *))
{
    hm_help_resizing(hmap);
    HNode ** from = h_lookup(&hmap->h1, key_node, cmp);
    if(!from)
    {
        from = h_lookup(&hmap->h2, key_node, cmp);
    }
    return from? *from: nullptr;
}

static void hm_start_resizing(HMap * hmap)
{  
    assert(hmap->h2.tab == nullptr);
    //create bigger hashtable
    hmap->h2 = hmap->h1;
    h_init(&hmap->h1, (hmap->h1.mask + 1) * 2);
    hmap->resizing_pos = 0;
}

void hm_insert(HMap *hmap, HNode * hnode)
{
    if(!hmap->h1.tab)
    {
        h_init(&hmap->h1, 4);
    }
    h_insert(&hmap->h1, hnode);

    if(!hmap->h2.tab)
    {
        size_t load_factor = hmap->h1.size / (hmap->h1.mask + 1);
        if(load_factor >= k_max_load_factor)
        {
            hm_start_resizing(hmap);
        }
    }
    hm_help_resizing(hmap);
}

HNode * hm_pop(HMap * hmap, HNode * key_node, bool(*cmp)(HNode *, HNode *))
{
    hm_help_resizing(hmap);
    HNode **from = h_lookup(&hmap->h1, key_node, cmp);
    if(from)
    {
        return h_detach(&hmap->h1, from);
    }
    from = h_lookup(&hmap->h2, key_node, cmp);
    if(from)
    {
        return h_detach(&hmap->h2, from);
    }
    return nullptr;
}

size_t hm_size(HMap *hmap)
{
    return hmap->h1.size + hmap->h2.size;
}