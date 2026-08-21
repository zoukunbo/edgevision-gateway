#include "address.h"

#include <string.h>
#include <arpa/inet.h>

int net_address_ipv4(
    net_address_t *address,
    const char *ip,
    uint16_t port)
{
    int rc;

    if (address == NULL || ip == NULL) {
        return -1;
    }

    memset(&address->addr, 0, sizeof(address->addr));


    address->addr.sin_family = AF_INET;
    address->addr.sin_port = htons(port);



    rc = inet_pton(AF_INET, ip,&address->addr.sin_addr);
    if (rc != 1)
    {
       return -1;
    }



    return 0;
}


const struct sockaddr *net_address_sockaddr(
    const net_address_t *address)
{
    /*
     *
     * 否则：
     *
     * &address->addr
     *
     * 从：
     * struct sockaddr_in *
     *
     * 转换为：
     * const struct sockaddr *
     */

     if (address == NULL)
     {
        return NULL;
     }

    return (const struct sockaddr*)&address->addr;

}

socklen_t net_address_length(
    const net_address_t *address)
{
    /*
     * TODO
     *
     * address == NULL 怎么处理？
     *
     * 正常情况：
     *
     * sizeof(address->addr)
     */
    if (address == NULL)
    {
        return 0;
    }
    return sizeof(address->addr);
}

struct sockaddr *net_address_sockaddr_mut(
    net_address_t *address)
{
    if (address == NULL)
    {
        return NULL;
    }

    return (struct sockaddr *)&address->addr;
}