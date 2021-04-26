/**
 * Copyright 2019, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Gracht Server Type Definitions & Structures
 * - This header describes the base server-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#include <errno.h>
#include "include/aio.h"
#include "include/arena.h"
#include "include/debug.h"
#include "include/gracht/server.h"
#include "include/gracht/link/link.h"
#include "include/thread_api.h"
#include "include/utils.h"
#include "include/server_private.h"
#include "include/hashtable.h"
#include "include/control.h"
#include "include/compiler.h"
#include <stdlib.h>
#include <string.h>

// Memory requirements of the server
// Single-threaded
// 1 buffer for incoming messages
// 1 buffer for outgoing events/responses
// Multi-threaded (M threads)
// N buffers for incoming messages
// M buffers for outgoing events/responses

struct client_wrapper {
    gracht_conn_t                handle;
    struct gracht_server_client* client;
};

struct broadcast_context {
    struct gracht_buffer* message;
    unsigned int          flags;
};

struct server_operations {
    void                        (*dispatch)(struct gracht_server*, struct gracht_recv_message*);
    void*                       (*get_outgoing_buffer)(struct gracht_server*);
    struct gracht_recv_message* (*get_incoming_buffer)(struct gracht_server*);
    void                        (*put_message)(struct gracht_server*, struct gracht_recv_message*);
};

struct gracht_server {
    struct server_operations*      ops;
    struct server_link_ops*        link;
    struct gracht_server_callbacks callbacks;
    struct gracht_arena*           arena;
    struct gracht_worker_pool*     worker_pool;
    size_t                         allocationSize;
    void*                          sendBuffer;
    void*                          recvBuffer;
    int                            initialized;
    gracht_handle_t                set_handle;
    int                            set_handle_provided;
    gracht_conn_t                  listen_handle;
    gracht_conn_t                  dgram_handle;
    mtx_t                          sync_object;
    hashtable_t                    protocols;
    hashtable_t                    clients;
} g_grachtServer = {
        NULL,
        NULL,
        { NULL, NULL },
        NULL,
        NULL,
        0,
        NULL,
        NULL,
        0,
        GRACHT_HANDLE_INVALID,
        0,
        GRACHT_CONN_INVALID,
        GRACHT_CONN_INVALID,
        { { 0 } },
        { 0 },
        { 0 }
};

static void*                       get_out_buffer_st(struct gracht_server*);
static struct gracht_recv_message* get_in_buffer_st(struct gracht_server*);
static void                        put_message_st(struct gracht_server*, struct gracht_recv_message*);
static void                        dispatch_st(struct gracht_server*, struct gracht_recv_message*);

static struct server_operations g_stOperations = {
    dispatch_st,
    get_out_buffer_st,
    get_in_buffer_st,
    put_message_st
};

static void*                       get_out_buffer_mt(struct gracht_server*);
static struct gracht_recv_message* get_in_buffer_mt(struct gracht_server*);
static void                        put_message_mt(struct gracht_server*, struct gracht_recv_message*);
static void                        dispatch_mt(struct gracht_server*, struct gracht_recv_message*);

static struct server_operations g_mtOperations = {
    dispatch_mt,
    get_out_buffer_mt,
    get_in_buffer_mt,
    put_message_mt
};

static void client_destroy(struct gracht_server*, gracht_conn_t);
static void client_subscribe(struct gracht_server_client*, uint8_t);
static void client_unsubscribe(struct gracht_server_client*, uint8_t);
static int  client_is_subscribed(struct gracht_server_client*, uint8_t);

static uint64_t client_hash(const void*);
static int      client_cmp(const void*, const void*);
static void     client_enum_destroy(int index, const void* element, void* userContext);
static void     client_enum_broadcast(int index, const void* element, void* userContext);


static int configure_server(struct gracht_server*, gracht_server_configuration_t*);
static int create_links(struct gracht_server*);

int gracht_server_initialize(gracht_server_configuration_t* configuration)
{
    int status;
    if (g_grachtServer.initialized) {
        errno = EALREADY;
        return -1;
    }

    if (!configuration) {
        errno = EINVAL;
        return -1;
    }

    mtx_init(&g_grachtServer.sync_object, mtx_plain);
    status = configure_server(&g_grachtServer, configuration);
    if (status) {
        GRERROR(GRSTR("gracht_server_initialize: invalid configuration provided"));
        return -1;
    }

    status = create_links(&g_grachtServer);
    if (status) {
        GRERROR(GRSTR("gracht_server_initialize: failed to initialize underlying links"));
        return -1;
    }

    hashtable_construct(&g_grachtServer.protocols, 0, sizeof(struct gracht_protocol), protocol_hash, protocol_cmp);
    hashtable_construct(&g_grachtServer.clients, 0, sizeof(struct client_wrapper), client_hash, client_cmp);

    gracht_server_register_protocol(&gracht_control_server_protocol);
    g_grachtServer.initialized = 1;
    return 0;
}

static int configure_server(struct gracht_server* server, gracht_server_configuration_t* configuration)
{
    size_t arenaSize;
    int    status;

    // set the configuration params that are just transfer
    server->link = configuration->link;
    memcpy(&server->callbacks, &configuration->callbacks, sizeof(struct gracht_server_callbacks));

    // handle the aio descriptor
    if (configuration->set_descriptor_provided) {
        server->set_handle          = configuration->set_descriptor;
        server->set_handle_provided = 1;
    }
    else {
        server->set_handle = gracht_aio_create();
        if (server->set_handle == GRACHT_HANDLE_INVALID) {
            GRERROR(GRSTR("gracht_server: failed to create aio handle"));
            return -1;
        }
    }

    // configure the allocation size, we use the max message size and add
    // 512 bytes for context data
    server->allocationSize = configuration->max_message_size + 512;

    // handle the worker count, if the worker count is not provided we do not use
    // the dispatcher, but instead handle single-threaded.
    if (configuration->server_workers > 1) {
        status = gracht_worker_pool_create(server, configuration->server_workers, server->allocationSize, &server->worker_pool);
        if (status) {
            GRERROR(GRSTR("configure_server: failed to create the worker pool"));
            return -1;
        }
        server->ops = &g_mtOperations;
    }
    else {
        server->ops = &g_stOperations;
    }

    // handle the max message size override, otherwise we default to our default value.
    if (configuration->server_workers > 1) {
        arenaSize = configuration->server_workers * server->allocationSize * 32;
        status    = gracht_arena_create(arenaSize, &server->arena);
        if (status) {
            GRERROR(GRSTR("configure_server: failed to create the memory pool"));
            return -1;
        }
    }
    else {
        server->sendBuffer = malloc(server->allocationSize);
        if (!server->sendBuffer) {
            GRERROR(GRSTR("configure_server: failed to allocate memory for outoing messages"));
            return -1;
        }
        
        server->recvBuffer = malloc(server->allocationSize);
        if (!server->recvBuffer) {
            GRERROR(GRSTR("configure_server: failed to allocate memory for incoming messages"));
            return -1;
        }
    }

    return 0;
}

static int create_links(struct gracht_server* server)
{
    // try to create the listening link. We do support that one of the links
    // are not supported by the link operations.
    server->listen_handle = server->link->listen(server->link, LINK_LISTEN_SOCKET);
    if (server->listen_handle == GRACHT_CONN_INVALID) {
        if (errno != ENOTSUP) {
            return -1;
        }
    }
    else {
        gracht_aio_add(server->set_handle, server->listen_handle);
    }

    server->dgram_handle = server->link->listen(server->link, LINK_LISTEN_DGRAM);
    if (server->dgram_handle == GRACHT_CONN_INVALID) {
        if (errno != ENOTSUP) {
            return -1;
        }
    }
    else {
        gracht_aio_add(server->set_handle, server->dgram_handle);
    }

    if (server->listen_handle == GRACHT_CONN_INVALID && server->dgram_handle == GRACHT_CONN_INVALID) {
        GRERROR(GRSTR("create_links: neither of client and dgram links were supported"));
        return -1;
    }
    return 0;
}

static int handle_client_socket(struct gracht_server* server)
{
    struct gracht_server_client* client;

    int status = server->link->accept(server->link, &client);
    if (status) {
        GRERROR(GRSTR("gracht_server: failed to accept client"));
        return status;
    }
    
    hashtable_set(&server->clients, &(struct client_wrapper){ .handle = client->handle, .client = client });
    gracht_aio_add(server->set_handle, client->handle);

    // invoke the new client callback at last
    if (server->callbacks.clientConnected) {
        server->callbacks.clientConnected(client->handle);
    }
    return 0;
}

static void* get_out_buffer_st(struct gracht_server* server)
{
    return server->sendBuffer;
}

static struct gracht_recv_message* get_in_buffer_st(struct gracht_server* server)
{
    return (struct gracht_recv_message*)server->recvBuffer;
}

static void put_message_st(struct gracht_server* server, struct gracht_recv_message* message)
{
    (void)server;
    (void)message;
}

static void dispatch_st(struct gracht_server* server, struct gracht_recv_message* message)
{
    server_invoke_action(server, message);
}

static void dispatch_mt(struct gracht_server* server, struct gracht_recv_message* message)
{
    uint32_t messageLength = *((uint32_t*)&message->payload[message->index + 4]);
    GRTRACE(GRSTR("dispatch_mt: message length=%u"), messageLength);

    // @todo debug why this ends up overwriting the memory
    //mtx_lock(&server->sync_object);
    //gracht_arena_free(server->arena, message, server->allocationSize - messageLength);
    //mtx_unlock(&server->sync_object);
    gracht_worker_pool_dispatch(server->worker_pool, message);
}

static void* get_out_buffer_mt(struct gracht_server* server)
{
    return gracht_worker_pool_get_worker_scratchpad(server->worker_pool);
}

static struct gracht_recv_message* get_in_buffer_mt(struct gracht_server* server)
{
    struct gracht_recv_message* message;
    mtx_lock(&server->sync_object);
    message = gracht_arena_allocate(server->arena, NULL, server->allocationSize);
    mtx_unlock(&server->sync_object);
    return message;
}

static void put_message_mt(struct gracht_server* server, struct gracht_recv_message* message)
{
    mtx_lock(&server->sync_object);
    gracht_arena_free(server->arena, message, server->allocationSize);
    mtx_unlock(&server->sync_object);
}

static int handle_sync_event(struct gracht_server* server)
{
    int status;
    GRTRACE(GRSTR("[handle_sync_event]"));
    
    while (1) {
        struct gracht_recv_message* message = server->ops->get_incoming_buffer(server);

        status = server->link->recv_packet(server->link, message, 0);
        if (status) {
            if (errno != ENODATA) {
                GRERROR(GRSTR("[handle_sync_event] server_object.link->recv_packet returned %i"), errno);
            }
            server->ops->put_message(server, message);
            break;
        }
        server->ops->dispatch(server, message);
    }
    
    return status;
}

static int handle_async_event(struct gracht_server* server, gracht_conn_t handle, uint32_t events)
{
    int status;
    GRTRACE(GRSTR("[handle_async_event] %i, 0x%x"), handle, events);
    
    // Check for control event. On non-passive sockets, control event is the
    // disconnect event.
    if (events & GRACHT_AIO_EVENT_DISCONNECT) {
        status = gracht_aio_remove(server->set_handle, handle);
        if (status) {
            GRWARNING(GRSTR("handle_async_event: failed to remove descriptor from aio"));
        }
        
        client_destroy(server, handle);
    }
    else if ((events & GRACHT_AIO_EVENT_IN) || !events) {
        struct client_wrapper* entry;
        
        entry = hashtable_get(&server->clients, &(struct client_wrapper){ .handle = handle });
        while (entry) {
            struct gracht_recv_message* message = server->ops->get_incoming_buffer(server);
            
            status = server->link->recv_client(entry->client, message, 0);
            if (status) {
                if (errno != ENODATA) {
                    GRERROR(GRSTR("[handle_async_event] server_object.link->recv_client returned %i"), errno);
                }
                server->ops->put_message(server, message);
                break;
            }

            server->ops->dispatch(server, message);
        }
    }
    return 0;
}

static int gracht_server_shutdown(void)
{
    if (!g_grachtServer.initialized) {
        errno = ENOTSUP;
        return -1;
    }
    
    hashtable_enumerate(&g_grachtServer.clients, client_enum_destroy, NULL);
    
    if (g_grachtServer.set_handle != GRACHT_HANDLE_INVALID && !g_grachtServer.set_handle_provided) {
        gracht_aio_destroy(g_grachtServer.set_handle);
    }

    if (g_grachtServer.worker_pool) {
        gracht_worker_pool_destroy(g_grachtServer.worker_pool);
    }
    
    hashtable_destroy(&g_grachtServer.protocols);
    hashtable_destroy(&g_grachtServer.clients);

    if (g_grachtServer.arena) {
        gracht_arena_destroy(g_grachtServer.arena);
    }

    if (g_grachtServer.sendBuffer) {
        free(g_grachtServer.sendBuffer);
    }
    
    if (g_grachtServer.recvBuffer) {
        free(g_grachtServer.recvBuffer);
    }
    
    if (g_grachtServer.link) {
        g_grachtServer.link->destroy(g_grachtServer.link);
    }

    g_grachtServer.initialized = 0;
    return 0;
}

void server_invoke_action(struct gracht_server* server, struct gracht_recv_message* recvMessage)
{
    gracht_protocol_function_t* function;
    gracht_buffer_t             buffer = { .data = (char*)&recvMessage->payload[0], .index = recvMessage->index };
    uint32_t                    messageId;
    uint8_t                     protocol;
    uint8_t                     action;

    smp_mb();
    messageId = *((uint32_t*)&buffer.data[buffer.index + 0]);
    protocol  = *((uint8_t*)&buffer.data[buffer.index + 8]);
    action    = *((uint8_t*)&buffer.data[buffer.index + 9]);

    mtx_lock(&server->sync_object);
    function = get_protocol_action(&server->protocols, protocol, action);
    mtx_unlock(&server->sync_object);
    if (!function) {
        GRWARNING(GRSTR("[dispatch_st] failed to invoke server action"));
        gracht_control_event_error_single(recvMessage->client, messageId, ENOENT);
        return;
    }

    // skip the message header when invoking
    buffer.index += GRACHT_MESSAGE_HEADER_SIZE;
    ((server_invoke_t)function->address)(recvMessage, &buffer);
}

void server_cleanup_message(struct gracht_server* server, struct gracht_recv_message* recvMessage)
{
    if (!server || !recvMessage) {
        return;
    }

    mtx_lock(&server->sync_object);
    gracht_arena_free(server->arena, recvMessage, 0);
    mtx_unlock(&server->sync_object);
}

int gracht_server_handle_event(gracht_conn_t handle, unsigned int events)
{
    if (handle == g_grachtServer.listen_handle) {
        return handle_client_socket(&g_grachtServer);
    }
    else if (handle == g_grachtServer.dgram_handle) {
        return handle_sync_event(&g_grachtServer);
    }
    else {
        return handle_async_event(&g_grachtServer, handle, events);
    }
}

int gracht_server_main_loop(void)
{
    gracht_aio_event_t events[32];
    int                i;

    GRTRACE(GRSTR("gracht_server: started... [%i, %i]"), g_grachtServer.listen_handle, g_grachtServer.dgram_handle);
    while (g_grachtServer.initialized) {
        int num_events = gracht_io_wait(g_grachtServer.set_handle, &events[0], 32);
        GRTRACE(GRSTR("gracht_server: %i events received!"), num_events);
        for (i = 0; i < num_events; i++) {
            gracht_conn_t handle = gracht_aio_event_handle(&events[i]);
            uint32_t      flags  = gracht_aio_event_events(&events[i]);

            GRTRACE(GRSTR("gracht_server: event %u from %i"), flags, handle);
            gracht_server_handle_event(handle, flags);
        }
    }

    return gracht_server_shutdown();
}

int gracht_server_get_buffer(gracht_buffer_t* buffer)
{
    // this should always return a safe buffer to use for the request
    buffer->data  = g_grachtServer.ops->get_outgoing_buffer(&g_grachtServer);
    buffer->index = 0;
    return 0;
}

int gracht_server_respond(struct gracht_recv_message* messageContext, gracht_buffer_t* message)
{
    struct client_wrapper* entry;
    int                    status;

    if (!messageContext || !message) {
        GRERROR(GRSTR("gracht_server: null message or context"));
        errno = (EINVAL);
        return -1;
    }

    // update message header
    ((uint32_t*)message->data)[0] = ((uint32_t*)messageContext->payload)[0];
    ((uint32_t*)message->data)[1] = message->index;

    entry = hashtable_get(&g_grachtServer.clients, &(struct client_wrapper){ .handle = messageContext->client });
    if (!entry) {
        status = g_grachtServer.link->respond(g_grachtServer.link, messageContext, message);
    }
    else {
        status = g_grachtServer.link->send_client(entry->client, message, GRACHT_MESSAGE_BLOCK);
    }
    return status;
}

int gracht_server_send_event(gracht_conn_t client, gracht_buffer_t* message, unsigned int flags)
{
    struct client_wrapper* clientEntry;
    int                    status;

    // update message header
    //((uint32_t*)message->data)[0] = 0;
    ((uint32_t*)message->data)[1] = message->index;

    clientEntry = hashtable_get(&g_grachtServer.clients, &(struct client_wrapper){ .handle = client });
    if (!clientEntry) {
        errno = (ENOENT);
        return -1;
    }
    
    // When sending target specific events - we do not care about subscriptions
    status = g_grachtServer.link->send_client(clientEntry->client, message, flags);
    return status;
}

int gracht_server_broadcast_event(gracht_buffer_t* message, unsigned int flags)
{
    struct broadcast_context context = {
        .message = message,
        .flags   = flags
    };

    // update message header
    //((uint32_t*)message->data)[0] = 0;
    ((uint32_t*)message->data)[1] = message->index;

    hashtable_enumerate(&g_grachtServer.clients, client_enum_broadcast, &context);
    return 0;
}

int gracht_server_register_protocol(gracht_protocol_t* protocol)
{
    if (!protocol) {
        errno = (EINVAL);
        return -1;
    }
    
    hashtable_set(&g_grachtServer.protocols, protocol);
    return 0;
}

void gracht_server_unregister_protocol(gracht_protocol_t* protocol)
{
    if (!protocol) {
        return;
    }
    
    hashtable_remove(&g_grachtServer.protocols, protocol);
}

gracht_conn_t gracht_server_get_dgram_iod(void)
{
    return g_grachtServer.dgram_handle;
}

gracht_handle_t gracht_server_get_set_iod(void)
{
    return g_grachtServer.set_handle;
}

// Client helpers
static void client_destroy(struct gracht_server* server, gracht_conn_t client)
{
    struct client_wrapper* entry;

    if (server->callbacks.clientDisconnected) {
        server->callbacks.clientDisconnected(client);
    }

    entry = hashtable_remove(&server->clients, &(struct client_wrapper){ .handle = client });
    if (!entry) {
        return;
    }
    server->link->destroy_client(entry->client);
}

// Client subscription helpers
static void client_subscribe(struct gracht_server_client* client, uint8_t id)
{
    int block  = id / 32;
    int offset = id % 32;

    if (id == 0xFF) {
        // subscribe to all
        memset(&client->subscriptions[0], 0xFF, sizeof(client->subscriptions));
        return;
    }

    client->subscriptions[block] |= (1 << offset);
}

static void client_unsubscribe(struct gracht_server_client* client, uint8_t id)
{
    int block  = id / 32;
    int offset = id % 32;

    if (id == 0xFF) {
        // unsubscribe to all
        memset(&client->subscriptions[0], 0, sizeof(client->subscriptions));
        return;
    }

    client->subscriptions[block] &= ~(1 << offset);
}

static int client_is_subscribed(struct gracht_server_client* client, uint8_t id)
{
    int block  = id / 32;
    int offset = id % 32;
    return (client->subscriptions[block] & (1 << offset)) != 0;
}

// Server control protocol implementation
void gracht_control_subscribe_invocation(const struct gracht_recv_message* message, const uint8_t protocol)
{
    struct client_wrapper* entry;
    
    entry = hashtable_get(&g_grachtServer.clients, &(struct client_wrapper){ .handle = message->client });
    if (!entry) {
        struct client_wrapper newEntry;
        if (g_grachtServer.link->create_client(g_grachtServer.link, (struct gracht_recv_message*)message, &newEntry.client)) {
            GRERROR(GRSTR("[gracht_control_subscribe_invocation] server_object.link->create_client returned error"));
            return;
        }

        newEntry.handle = message->client;
        hashtable_set(&g_grachtServer.clients, &newEntry);

        if (g_grachtServer.callbacks.clientConnected) {
            g_grachtServer.callbacks.clientConnected(message->client);
        }
    }

    client_subscribe(entry->client, protocol);
}

void gracht_control_unsubscribe_invocation(const struct gracht_recv_message* message, const uint8_t protocol)
{
    struct client_wrapper* entry;
    
    entry = hashtable_get(&g_grachtServer.clients, &(struct client_wrapper){ .handle = message->client });
    if (!entry) {
        return;
    }

    client_unsubscribe(entry->client, protocol);
    
    // cleanup the client if we unsubscripe
    if (protocol == 0xFF) {
        client_destroy(&g_grachtServer, message->client);
    }
}

static uint64_t client_hash(const void* element)
{
    const struct client_wrapper* client = element;
    return (uint64_t)client->handle;
}

static int client_cmp(const void* element1, const void* element2)
{
    const struct client_wrapper* client1 = element1;
    const struct client_wrapper* client2 = element2;
    return client1->handle == client2->handle ? 0 : 1;
}

static void client_enum_broadcast(int index, const void* element, void* userContext)
{
    const struct client_wrapper* entry   = element;
    struct broadcast_context*    context = userContext;
    uint8_t                      protocol = (uint8_t)context->message->data[8];
    (void)index;

    if (client_is_subscribed(entry->client, protocol)) {
        g_grachtServer.link->send_client(entry->client, context->message, context->flags);
    }
}

static void client_enum_destroy(int index, const void* element, void* userContext)
{
    const struct client_wrapper* entry = element;
    (void)index;
    (void)userContext;

    g_grachtServer.link->destroy_client(entry->client);
}
