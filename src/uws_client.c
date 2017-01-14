 // Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <stdint.h>
#include <ctype.h>
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/uws_client.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/xio.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/socketio.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/tlsio.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/uws_frame_encoder.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/utf8_checker.h"

/* Requirements not needed as they are optional:
Codes_SRS_UWS_CLIENT_01_254: [ If an endpoint receives a Ping frame and has not yet sent Pong frame(s) in response to previous Ping frame(s), the endpoint MAY elect to send a Pong frame for only the most recently processed Ping frame. ]
Codes_SRS_UWS_CLIENT_01_255: [ A Pong frame MAY be sent unsolicited. ]
Codes_SRS_UWS_CLIENT_01_256: [ A response to an unsolicited Pong frame is not expected. ]
*/

/* Requirements satisfied by the underlying TLS/socket stack
Codes_SRS_UWS_CLIENT_01_362: [ To achieve reasonable levels of protection, clients should use only Strong TLS algorithms. ]
Codes_SRS_UWS_CLIENT_01_289: [ An endpoint SHOULD use a method that cleanly closes the TCP connection, as well as the TLS session, if applicable, discarding any trailing bytes that may have been received. ]
Codes_SRS_UWS_CLIENT_01_078: [ Otherwise, all further communication on this channel MUST run through the encrypted tunnel [RFC5246]. ]
Codes_SRS_UWS_CLIENT_01_141: [ masking is done whether or not the WebSocket Protocol is running over TLS. ]
*/

typedef enum UWS_STATE_TAG
{
    UWS_STATE_CLOSED,
    UWS_STATE_OPENING_UNDERLYING_IO,
    UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE,
    UWS_STATE_OPEN,
    UWS_STATE_CLOSING_SENDING_CLOSE,
    UWS_STATE_CLOSING_UNDERLYING_IO,
    UWS_STATE_ERROR
} UWS_STATE;

typedef enum UWS_FRAME_DECODER_STATE_TAG
{
    UWS_FRAME_DECODER_STATE_HEADER_AND_LENGTH,
    UWS_FRAME_DECODER_STATE_EXTENDED_LENGTH_16,
    UWS_FRAME_DECODER_STATE_EXTENDED_LENGTH_64,
    UWS_FRAME_DECODER_STATE_PAYLOAD_BYTES
} UWS_FRAME_DECODER_STATE;

typedef struct WS_INSTANCE_PROTOCOL_TAG
{
    char* protocol;
} WS_INSTANCE_PROTOCOL;

typedef struct WS_PENDING_SEND_TAG
{
    ON_WS_SEND_FRAME_COMPLETE on_ws_send_frame_complete;
    void* context;
    UWS_CLIENT_HANDLE uws_client;
} WS_PENDING_SEND;

typedef struct UWS_CLIENT_INSTANCE_TAG
{
    SINGLYLINKEDLIST_HANDLE pending_sends;
    XIO_HANDLE underlying_io;
    char* hostname;
    char* resource_name;
    WS_INSTANCE_PROTOCOL* protocols;
    size_t protocol_count;
    int port;
    UWS_STATE uws_state;
    ON_WS_OPEN_COMPLETE on_ws_open_complete;
    void* on_ws_open_complete_context;
    ON_WS_FRAME_RECEIVED on_ws_frame_received;
    void* on_ws_frame_received_context;
    ON_WS_PEER_CLOSED on_ws_peer_closed;
    void* on_ws_peer_closed_context;
    ON_WS_ERROR on_ws_error;
    void* on_ws_error_context;
    ON_WS_CLOSE_COMPLETE on_ws_close_complete;
    void* on_ws_close_complete_context;
    unsigned char* received_bytes;
    size_t received_bytes_count;
    UWS_FRAME_DECODER_STATE frame_decoder_state;
    BUFFER_HANDLE encode_buffer;
} UWS_CLIENT_INSTANCE;

/* Codes_SRS_UWS_CLIENT_01_360: [ Connection confidentiality and integrity is provided by running the WebSocket Protocol over TLS (wss URIs). ]*/
/* Codes_SRS_UWS_CLIENT_01_361: [ WebSocket implementations MUST support TLS and SHOULD employ it when communicating with their peers. ]*/
/* Codes_SRS_UWS_CLIENT_01_063: [ A client will need to supply a /host/, /port/, /resource name/, and a /secure/ flag, which are the components of a WebSocket URI as discussed in Section 3, along with a list of /protocols/ and /extensions/ to be used. ]*/
UWS_CLIENT_HANDLE uws_client_create(const char* hostname, unsigned int port, const char* resource_name, bool use_ssl, const WS_PROTOCOL* protocols, size_t protocol_count)
{
    UWS_CLIENT_HANDLE result;

    /* Codes_SRS_UWS_CLIENT_01_002: [ If any of the arguments `hostname` and `resource_name` is NULL then `uws_client_create` shall return NULL. ]*/
    if ((hostname == NULL) ||
        (resource_name == NULL) ||
        /* Codes_SRS_UWS_CLIENT_01_411: [ If `protocol_count` is non zero and `protocols` is NULL then `uws_client_create` shall fail and return NULL. ]*/
        ((protocols == NULL) && (protocol_count > 0)))
    {
        LogError("Invalid arguments: hostname = %p, resource_name = %p, protocols = %p, protocol_count = %zu", hostname, resource_name, protocols, protocol_count);
        result = NULL;
    }
    else
    {
        /* Codes_SRS_UWS_CLIENT_01_412: [ If the `protocol` member of any of the items in the `protocols` argument is NULL, then `uws_client_create` shall fail and return NULL. ]*/
        size_t i;
        for (i = 0; i < protocol_count; i++)
        {
            if (protocols[i].protocol == NULL)
            {
                break;
            }
        }

        if (i < protocol_count)
        {
            LogError("Protocol index %zu has NULL name", i);
            result = NULL;
        }
        else
        {
            /* Codes_SRS_UWS_CLIENT_01_001: [`uws_client_create` shall create an instance of uws and return a non-NULL handle to it.]*/
            result = malloc(sizeof(UWS_CLIENT_INSTANCE));
            if (result == NULL)
            {
                /* Codes_SRS_UWS_CLIENT_01_003: [ If allocating memory for the new uws instance fails then `uws_client_create` shall return NULL. ]*/
                LogError("Could not allocate uWS instance");
            }
            else
            {
                /* Codes_SRS_UWS_CLIENT_01_422: [ `uws_client_create` shall create a buffer to be used for encoding outgoing frames by calling `BUFFER_new`. ]*/
                result->encode_buffer = BUFFER_new();
                if (result->encode_buffer == NULL)
                {
                    /* Codes_SRS_UWS_CLIENT_01_423: [ If `BUFFER_new` fails  then `uws_client_create` shall fail and return NULL. ]*/
                    LogError("Could not allocate encode buffer.");
                    free(result);
                    result = NULL;
                }
                else
                {
                    /* Codes_SRS_UWS_CLIENT_01_004: [ The argument `hostname` shall be copied for later use. ]*/
                    if (mallocAndStrcpy_s(&result->hostname, hostname) != 0)
                    {
                        /* Codes_SRS_UWS_CLIENT_01_392: [ If allocating memory for the copy of the `hostname` argument fails, then `uws_client_create` shall return NULL. ]*/
                        LogError("Could not copy hostname.");
                        BUFFER_delete(result->encode_buffer);
                        free(result);
                        result = NULL;
                    }
                    else
                    {
                        /* Codes_SRS_UWS_CLIENT_01_404: [ The argument `resource_name` shall be copied for later use. ]*/
                        if (mallocAndStrcpy_s(&result->resource_name, resource_name) != 0)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_405: [ If allocating memory for the copy of the `resource` argument fails, then `uws_client_create` shall return NULL. ]*/
                            LogError("Could not copy resource.");
                            free(result->hostname);
                            BUFFER_delete(result->encode_buffer);
                            free(result);
                            result = NULL;
                        }
                        else
                        {
                            /* Codes_SRS_UWS_CLIENT_01_017: [ `uws_client_create` shall create a pending send IO list that is to be used to queue send packets by calling `singlylinkedlist_create`. ]*/
                            result->pending_sends = singlylinkedlist_create();
                            if (result->pending_sends == NULL)
                            {
                                /* Codes_SRS_UWS_CLIENT_01_018: [ If `singlylinkedlist_create` fails then `uws_client_create` shall fail and return NULL. ]*/
                                LogError("Could not allocate pending send frames list");
                                free(result->resource_name);
                                free(result->hostname);
                                BUFFER_delete(result->encode_buffer);
                                free(result);
                                result = NULL;
                            }
                            else
                            {
                                if (use_ssl == true)
                                {
                                    TLSIO_CONFIG tlsio_config;

                                    /* Codes_SRS_UWS_CLIENT_01_006: [ If `use_ssl` is 1 then `uws_client_create` shall obtain the interface used to create a tlsio instance by calling `platform_get_default_tlsio`. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_076: [ If /secure/ is true, the client MUST perform a TLS handshake over the connection after opening the connection and before sending the handshake data [RFC2818]. ]*/
                                    const IO_INTERFACE_DESCRIPTION* tlsio_interface = platform_get_default_tlsio();
                                    if (tlsio_interface == NULL)
                                    {
                                        /* Codes_SRS_UWS_CLIENT_01_007: [ If obtaining the underlying IO interface fails, then `uws_client_create` shall fail and return NULL. ]*/
                                        LogError("NULL TLSIO interface description");
                                        result->underlying_io = NULL;
                                    }
                                    else
                                    {
                                        /* Codes_SRS_UWS_CLIENT_01_013: [ The create arguments for the tls IO (when `use_ssl` is 1) shall have: ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_014: [ - `hostname` set to the `hostname` argument passed to `uws_client_create`. ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_015: [ - `port` set to the `port` argument passed to `uws_client_create`. ]*/
                                        tlsio_config.hostname = hostname;
                                        tlsio_config.port = port;

                                        result->underlying_io = xio_create(tlsio_interface, &tlsio_config);
                                        if (result->underlying_io == NULL)
                                        {
                                            LogError("Cannot create underlying TLS IO.");
                                        }
                                    }
                                }
                                else
                                {
                                    SOCKETIO_CONFIG socketio_config;
                                    /* Codes_SRS_UWS_CLIENT_01_005: [ If `use_ssl` is 0 then `uws_client_create` shall obtain the interface used to create a socketio instance by calling `socketio_get_interface_description`. ]*/
                                    const IO_INTERFACE_DESCRIPTION* socketio_interface = socketio_get_interface_description();
                                    if (socketio_interface == NULL)
                                    {
                                        /* Codes_SRS_UWS_CLIENT_01_007: [ If obtaining the underlying IO interface fails, then `uws_client_create` shall fail and return NULL. ]*/
                                        LogError("NULL socketio interface description");
                                        result->underlying_io = NULL;
                                    }
                                    else
                                    {
                                        /* Codes_SRS_UWS_CLIENT_01_010: [ The create arguments for the socket IO (when `use_ssl` is 0) shall have: ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_011: [ - `hostname` set to the `hostname` argument passed to `uws_client_create`. ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_012: [ - `port` set to the `port` argument passed to `uws_client_create`. ]*/
                                        socketio_config.hostname = hostname;
                                        socketio_config.port = port;
                                        socketio_config.accepted_socket = NULL;

                                        /* Codes_SRS_UWS_CLIENT_01_008: [ The obtained interface shall be used to create the IO used as underlying IO by the newly created uws instance. ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_009: [ The underlying IO shall be created by calling `xio_create`. ]*/
                                        result->underlying_io = xio_create(socketio_interface, &socketio_config);
                                        if (result->underlying_io == NULL)
                                        {
                                            LogError("Cannot create underlying socket IO.");
                                        }
                                    }
                                }

                                if (result->underlying_io == NULL)
                                {
                                    /* Tests_SRS_UWS_CLIENT_01_016: [ If `xio_create` fails, then `uws_client_create` shall fail and return NULL. ]*/
                                    singlylinkedlist_destroy(result->pending_sends);
                                    free(result->resource_name);
                                    free(result->hostname);
                                    BUFFER_delete(result->encode_buffer);
                                    free(result);
                                    result = NULL;
                                }
                                else
                                {
                                    result->uws_state = UWS_STATE_CLOSED;
                                    /* Codes_SRS_UWS_CLIENT_01_403: [ The argument `port` shall be copied for later use. ]*/
                                    result->port = port;

                                    result->on_ws_open_complete = NULL;
                                    result->on_ws_open_complete_context = NULL;
                                    result->on_ws_frame_received = NULL;
                                    result->on_ws_frame_received_context = NULL;
                                    result->on_ws_error = NULL;
                                    result->on_ws_error_context = NULL;
                                    result->on_ws_close_complete = NULL;
                                    result->on_ws_close_complete_context = NULL;
                                    result->received_bytes = NULL;
                                    result->received_bytes_count = 0;

                                    result->protocol_count = protocol_count;

                                    /* Codes_SRS_UWS_CLIENT_01_410: [ The `protocols` argument shall be allowed to be NULL, in which case no protocol is to be specified by the client in the upgrade request. ]*/
                                    if (protocols != NULL)
                                    {
                                        result->protocols = (WS_INSTANCE_PROTOCOL*)malloc(sizeof(WS_INSTANCE_PROTOCOL) * protocol_count);
                                        if (result->protocols == NULL)
                                        {
                                            /* Codes_SRS_UWS_CLIENT_01_414: [ If allocating memory for the copied protocol information fails then `uws_client_create` shall fail and return NULL. ]*/
                                            LogError("Cannot allocate memory for the protocols array.");
                                            xio_destroy(result->underlying_io);
                                            singlylinkedlist_destroy(result->pending_sends);
                                            free(result->resource_name);
                                            free(result->hostname);
                                            BUFFER_delete(result->encode_buffer);
                                            free(result);
                                            result = NULL;
                                        }
                                        else
                                        {
                                            /* Codes_SRS_UWS_CLIENT_01_413: [ The protocol information indicated by `protocols` and `protocol_count` shall be copied for later use (for constructing the upgrade request). ]*/
                                            for (i = 0; i < protocol_count; i++)
                                            {
                                                if (mallocAndStrcpy_s(&result->protocols[i].protocol, protocols[i].protocol) != 0)
                                                {
                                                    /* Codes_SRS_UWS_CLIENT_01_414: [ If allocating memory for the copied protocol information fails then `uws_client_create` shall fail and return NULL. ]*/
                                                    LogError("Cannot allocate memory for the protocol index %u.", (unsigned int)i);
                                                    break;
                                                }
                                            }

                                            if (i < protocol_count)
                                            {
                                                size_t j;

                                                for (j = 0; j < i; j++)
                                                {
                                                    free(result->protocols[j].protocol);
                                                }

                                                free(result->protocols);
                                                xio_destroy(result->underlying_io);
                                                singlylinkedlist_destroy(result->pending_sends);
                                                free(result->resource_name);
                                                free(result->hostname);
                                                BUFFER_delete(result->encode_buffer);
                                                free(result);
                                                result = NULL;
                                            }
                                            else
                                            {
                                                result->protocol_count = protocol_count;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return result;
}

void uws_client_destroy(UWS_CLIENT_HANDLE uws_client)
{
    /* Codes_SRS_UWS_CLIENT_01_020: [ If `uws_client` is NULL, `uws_client_destroy` shall do nothing. ]*/
    if (uws_client == NULL)
    {
        LogError("NULL uws handle");
    }
    else
    {
        free(uws_client->received_bytes);

        /* Codes_SRS_UWS_CLIENT_01_021: [ `uws_client_destroy` shall perform a close action if the uws instance has already been open. ]*/
        switch (uws_client->uws_state)
        {
        default:
            break;

        case UWS_STATE_OPEN:
        case UWS_STATE_ERROR:
            uws_client_close(uws_client, NULL, NULL);
            break;
        }

        if (uws_client->protocol_count > 0)
        {
            size_t i;

            /* Codes_SRS_UWS_CLIENT_01_437: [ `uws_client_destroy` shall free the protocols array allocated in `uws_client_create`. ]*/
            for (i = 0; i < uws_client->protocol_count; i++)
            {
                free(uws_client->protocols[i].protocol);
            }

            free(uws_client->protocols);
        }

        /* Codes_SRS_UWS_CLIENT_01_019: [ `uws_client_destroy` shall free all resources associated with the uws instance. ]*/
        /* Codes_SRS_UWS_CLIENT_01_023: [ `uws_client_destroy` shall ensure the underlying IO created in `uws_client_open` is destroyed by calling `xio_destroy`. ]*/
        if (uws_client->underlying_io != NULL)
        {
            xio_destroy(uws_client->underlying_io);
            uws_client->underlying_io = NULL;
        }

        /* Codes_SRS_UWS_CLIENT_01_024: [ `uws_client_destroy` shall free the list used to track the pending sends by calling `singlylinkedlist_destroy`. ]*/
        singlylinkedlist_destroy(uws_client->pending_sends);
        free(uws_client->resource_name);
        free(uws_client->hostname);

        /* Codes_SRS_UWS_CLIENT_01_424: [ `uws_client_destroy` shall free the buffer allocated in `uws_client_create` by calling `BUFFER_delete`. ]*/
        BUFFER_delete(uws_client->encode_buffer);
        free(uws_client);
    }
}

static void indicate_ws_open_complete_error(UWS_CLIENT_INSTANCE* uws_client, WS_OPEN_RESULT ws_open_result)
{
    /* Codes_SRS_UWS_CLIENT_01_409: [ After any error is indicated by `on_ws_open_complete`, a subsequent `uws_client_open` shall be possible. ]*/
    uws_client->on_ws_open_complete(uws_client->on_ws_open_complete_context, ws_open_result);
    uws_client->uws_state = UWS_STATE_CLOSED;
}

static void indicate_ws_open_complete_error_and_close(UWS_CLIENT_INSTANCE* uws_client, WS_OPEN_RESULT ws_open_result)
{
    indicate_ws_open_complete_error(uws_client, ws_open_result);
    (void)xio_close(uws_client->underlying_io, NULL, NULL);
}

static void indicate_ws_error(UWS_CLIENT_INSTANCE* uws_client, WS_ERROR error_code)
{
    uws_client->uws_state = UWS_STATE_ERROR;
    uws_client->on_ws_error(uws_client->on_ws_error_context, error_code);
}

static void indicate_ws_close_complete(UWS_CLIENT_INSTANCE* uws_client)
{
    uws_client->uws_state = UWS_STATE_CLOSED;

    /* Codes_SRS_UWS_CLIENT_01_496: [ If the close was initiated by the peer no `on_ws_close_complete` shall be called. ]*/
    if (uws_client->on_ws_close_complete != NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_491: [ When calling `on_ws_close_complete` callback, the `on_ws_close_complete_context` argument shall be passed to it. ]*/
        uws_client->on_ws_close_complete(uws_client->on_ws_close_complete_context);
    }
}

static int send_close_frame(UWS_CLIENT_INSTANCE* uws_client, unsigned int close_error_code)
{
    unsigned char* close_frame;
    unsigned char close_frame_payload[2];
    size_t close_frame_length;
    int result;

    close_frame_payload[0] = (unsigned char)(close_error_code >> 8);
    close_frame_payload[1] = (unsigned char)(close_error_code & 0xFF);

    /* Codes_SRS_UWS_CLIENT_01_140: [ To avoid confusing network intermediaries (such as intercepting proxies) and for security reasons that are further discussed in Section 10.3, a client MUST mask all frames that it sends to the server (see Section 5.3 for further details). ]*/
    if (uws_frame_encoder_encode(uws_client->encode_buffer, WS_CLOSE_FRAME, close_frame_payload, sizeof(close_frame_payload), true, true, 0) != 0)
    {
        LogError("Encoding of CLOSE failed.");
        result = __LINE__;
    }
    else
    {
        close_frame = BUFFER_u_char(uws_client->encode_buffer);
        close_frame_length = BUFFER_length(uws_client->encode_buffer);

        /* Codes_SRS_UWS_CLIENT_01_471: [ The callback `on_underlying_io_close_sent` shall be passed as argument to `xio_send`. ]*/
        if (xio_send(uws_client->underlying_io, close_frame, close_frame_length, NULL, NULL) != 0)
        {
            LogError("Sending CLOSE frame failed.");
            result = __LINE__;
        }
        else
        {
            result = 0;
        }
    }

    return result;
}

static void indicate_ws_error_and_close(UWS_CLIENT_INSTANCE* uws_client, WS_ERROR error_code, unsigned int close_error_code)
{
    uws_client->uws_state = UWS_STATE_ERROR;

    (void)send_close_frame(uws_client, close_error_code);

    uws_client->on_ws_error(uws_client->on_ws_error_context, error_code);
}

static void on_underlying_io_open_complete(void* context, IO_OPEN_RESULT open_result)
{
    UWS_CLIENT_HANDLE uws_client = context;
    /* Codes_SRS_UWS_CLIENT_01_401: [ If `on_underlying_io_open_complete` is called with a NULL context, `on_underlying_io_open_complete` shall do nothing. ]*/
    if (uws_client == NULL)
    {
        LogError("NULL context");
    }
    else
    {
        switch (uws_client->uws_state)
        {
        default:
        case UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE:
            /* Codes_SRS_UWS_CLIENT_01_407: [ When `on_underlying_io_open_complete` is called when the uws instance has send the upgrade request but it is waiting for the response, an error shall be reported to the user by calling the `on_ws_open_complete` with `WS_OPEN_ERROR_MULTIPLE_UNDERLYING_IO_OPEN_EVENTS`. ]*/
            LogError("underlying on_io_open_complete was called again after upgrade request was sent.");
            indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_MULTIPLE_UNDERLYING_IO_OPEN_EVENTS);
            break;
        case UWS_STATE_OPENING_UNDERLYING_IO:
            switch (open_result)
            {
            default:
            case IO_OPEN_ERROR:
                /* Codes_SRS_UWS_CLIENT_01_369: [ When `on_underlying_io_open_complete` is called with `IO_OPEN_ERROR` while uws is OPENING (`uws_client_open` was called), uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_UNDERLYING_IO_OPEN_FAILED`. ]*/
                indicate_ws_open_complete_error(uws_client, WS_OPEN_ERROR_UNDERLYING_IO_OPEN_FAILED);
                break;
            case IO_OPEN_CANCELLED:
                /* Codes_SRS_UWS_CLIENT_01_402: [ When `on_underlying_io_open_complete` is called with `IO_OPEN_CANCELLED` while uws is OPENING (`uws_client_open` was called), uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_UNDERLYING_IO_OPEN_CANCELLED`. ]*/
                indicate_ws_open_complete_error(uws_client, WS_OPEN_ERROR_UNDERLYING_IO_OPEN_CANCELLED);
                break;
            case IO_OPEN_OK:
            {
                int upgrade_request_length;
                char* upgrade_request;

                /* Codes_SRS_UWS_CLIENT_01_371: [ When `on_underlying_io_open_complete` is called with `IO_OPEN_OK` while uws is OPENING (`uws_client_open` was called), uws shall prepare the WebSockets upgrade request. ]*/
                /* Codes_SRS_UWS_CLIENT_01_081: [ The handshake consists of an HTTP Upgrade request, along with a list of required and optional header fields. ]*/
                /* Codes_SRS_UWS_CLIENT_01_082: [ The handshake MUST be a valid HTTP request as specified by [RFC2616]. ]*/
                /* Codes_SRS_UWS_CLIENT_01_083: [ The method of the request MUST be GET, and the HTTP version MUST be at least 1.1. ]*/
                /* Codes_SRS_UWS_CLIENT_01_084: [ The "Request-URI" part of the request MUST match the /resource name/ defined in Section 3 (a relative URI) or be an absolute http/https URI that, when parsed, has a /resource name/, /host/, and /port/ that match the corresponding ws/wss URI. ]*/
                /* Codes_SRS_UWS_CLIENT_01_085: [ The request MUST contain a |Host| header field whose value contains /host/ plus optionally ":" followed by /port/ (when not using the default port). ]*/
                /* Codes_SRS_UWS_CLIENT_01_086: [ The request MUST contain an |Upgrade| header field whose value MUST include the "websocket" keyword. ]*/
                /* Codes_SRS_UWS_CLIENT_01_087: [ The request MUST contain a |Connection| header field whose value MUST include the "Upgrade" token. ]*/
                /* Codes_SRS_UWS_CLIENT_01_088: [ The request MUST include a header field with the name |Sec-WebSocket-Key|. ]*/
                /* Codes_SRS_UWS_CLIENT_01_094: [ The request MUST include a header field with the name |Sec-WebSocket-Version|. ]*/
                /* Codes_SRS_UWS_CLIENT_01_095: [ The value of this header field MUST be 13. ]*/
                /* Codes_SRS_UWS_CLIENT_01_096: [ The request MAY include a header field with the name |Sec-WebSocket-Protocol|. ]*/
                /* Codes_SRS_UWS_CLIENT_01_100: [ The request MAY include a header field with the name |Sec-WebSocket-Extensions|. ]*/
                const char upgrade_request_format[] = "GET %s HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                    "Sec-WebSocket-Protocol: %s\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    "\r\n";

                upgrade_request_length = snprintf(NULL, 0, upgrade_request_format,
                    uws_client->resource_name,
                    uws_client->hostname,
                    uws_client->port,
                    uws_client->protocols[0].protocol);
                if (upgrade_request_length < 0)
                {
                    /* Codes_SRS_UWS_CLIENT_01_408: [ If constructing of the WebSocket upgrade request fails, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_CONSTRUCTING_UPGRADE_REQUEST`. ]*/
                    LogError("Cannot construct the WebSocket upgrade request");
                    indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_CONSTRUCTING_UPGRADE_REQUEST);
                }
                else
                {
                    upgrade_request = (char*)malloc(upgrade_request_length + 1);
                    if (upgrade_request == NULL)
                    {
                        /* Codes_SRS_UWS_CLIENT_01_406: [ If not enough memory can be allocated to construct the WebSocket upgrade request, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_NOT_ENOUGH_MEMORY`. ]*/
                        LogError("Cannot allocate memory for the WebSocket upgrade request");
                        indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_NOT_ENOUGH_MEMORY);
                    }
                    else
                    {
                        upgrade_request_length = snprintf(upgrade_request, upgrade_request_length + 1, upgrade_request_format,
                            uws_client->resource_name,
                            uws_client->hostname,
                            uws_client->port,
                            uws_client->protocols[0].protocol);

                        /* No need to have any send complete here, as we are monitoring the received bytes */
                        /* Codes_SRS_UWS_CLIENT_01_372: [ Once prepared the WebSocket upgrade request shall be sent by calling `xio_send`. ]*/
                        /* Codes_SRS_UWS_CLIENT_01_080: [ Once a connection to the server has been established (including a connection via a proxy or over a TLS-encrypted tunnel), the client MUST send an opening handshake to the server. ]*/
                        if (xio_send(uws_client->underlying_io, upgrade_request, upgrade_request_length, NULL, NULL) != 0)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_373: [ If `xio_send` fails then uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_CANNOT_SEND_UPGRADE_REQUEST`. ]*/
                            LogError("Cannot send upgrade request");
                            indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_CANNOT_SEND_UPGRADE_REQUEST);
                        }
                        else
                        {
                            /* Codes_SRS_UWS_CLIENT_01_102: [ Once the client's opening handshake has been sent, the client MUST wait for a response from the server before sending any further data. ]*/
                            uws_client->uws_state = UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE;
                        }

                        free(upgrade_request);
                    }
                }

                break;
            }
            }
        }
    }
}

static void consume_received_bytes(UWS_CLIENT_INSTANCE* uws_client, size_t consumed_bytes)
{
    if (consumed_bytes < uws_client->received_bytes_count)
    {
        (void)memmove(uws_client->received_bytes, uws_client->received_bytes + consumed_bytes, uws_client->received_bytes_count - consumed_bytes);
    }

    uws_client->received_bytes_count -= consumed_bytes;
}

static void on_underlying_io_close_complete(void* context)
{
    if (context == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_477: [ When `on_underlying_io_close_complete` is called with a NULL context, it shall do nothing. ]*/
        LogError("NULL context for on_underlying_io_close_complete");
    }
    else
    {
        UWS_CLIENT_HANDLE uws_client = context;
        if (uws_client->uws_state == UWS_STATE_CLOSING_UNDERLYING_IO)
        {
            /* Codes_SRS_UWS_CLIENT_01_475: [ When `on_underlying_io_close_complete` is called while closing the underlying IO a subsequent `uws_client_open` shall succeed. ]*/
            uws_client->uws_state = UWS_STATE_CLOSED;
        }
    }
}

static void on_underlying_io_close_sent(void* context, IO_SEND_RESULT io_send_result)
{
    if (context == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_489: [ When `on_underlying_io_close_sent` is called with NULL context, it shall do nothing. ] */
        LogError("NULL context in ");
    }
    else
    {
        UWS_CLIENT_INSTANCE* uws_client = context;

        switch (io_send_result)
        {
        case IO_SEND_OK:
        case IO_SEND_CANCELLED:
            if (uws_client->uws_state == UWS_STATE_CLOSING_SENDING_CLOSE)
            {
                uws_client->uws_state = UWS_STATE_CLOSING_UNDERLYING_IO;

                /* Codes_SRS_UWS_CLIENT_01_490: [ When `on_underlying_io_close_sent` is called while the uws client is CLOSING, `on_underlying_io_close_sent` shall close the underlying IO by calling `xio_close`. ]*/
                if (xio_close(uws_client->underlying_io, on_underlying_io_close_complete, uws_client) != 0)
                {
                    /* Codes_SRS_UWS_CLIENT_01_496: [ If the close was initiated by the peer no `on_ws_close_complete` shall be called. ]*/
                    indicate_ws_close_complete(uws_client);
                }
            }

        case IO_SEND_ERROR:
            break;
        }
    }
}

/*the following function does the same as sscanf(pos2, "%d", &sec)*/
/*this function only exists because some of platforms do not have sscanf. */
static int ParseStringToDecimal(const char *src, int* dst)
{
    int result;
    char* next;

    (*dst) = strtol(src, &next, 0);
    if ((src == next) || ((((*dst) == LONG_MAX) || ((*dst) == LONG_MIN)) && (errno != 0)))
    {
        result = __LINE__;
    }
    else
    {
        result = 0;
    }

    return result;
}

/*the following function does the same as sscanf(buf, "HTTP/%*d.%*d %d %*[^\r\n]", &ret) */
/*this function only exists because some of platforms do not have sscanf. This is not a full implementation; it only works with well-defined HTTP response. */
static int ParseHttpResponse(const char* src, int* dst)
{
    int result;
    static const char HTTPPrefix[] = "HTTP/";
    bool fail;
    const char* runPrefix;

    if ((src == NULL) || (dst == NULL))
    {
        result = __LINE__;
    }
    else
    {
        fail = false;
        runPrefix = HTTPPrefix;

        while ((*runPrefix) != '\0')
        {
            if ((*runPrefix) != (*src))
            {
                fail = true;
                break;
            }
            src++;
            runPrefix++;
        }

        if (!fail)
        {
            while ((*src) != '.')
            {
                if ((*src) == '\0')
                {
                    fail = true;
                    break;
                }
                src++;
            }
        }

        if (!fail)
        {
            while ((*src) != ' ')
            {
                if ((*src) == '\0')
                {
                    fail = true;
                    break;
                }
                src++;
            }
        }

        if (fail)
        {
            result = __LINE__;
        }
        else
        {
            if (ParseStringToDecimal(src, dst) != 0)
            {
                result = __LINE__;
            }
            else
            {
                result = 0;
            }
        }
    }

    return result;
}

static void on_underlying_io_bytes_received(void* context, const unsigned char* buffer, size_t size)
{
    /* Codes_SRS_UWS_CLIENT_01_415: [ If called with a NULL `context` argument, `on_underlying_io_bytes_received` shall do nothing. ]*/
    if (context != NULL)
    {
        UWS_CLIENT_HANDLE uws_client = context;

        if ((buffer == NULL) ||
            (size == 0))
        {
            /* Codes_SRS_UWS_CLIENT_01_416: [ If called with NULL `buffer` or zero `size` and the state of the iws is OPENING, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_INVALID_BYTES_RECEIVED_ARGUMENTS`. ]*/
            indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_INVALID_BYTES_RECEIVED_ARGUMENTS);
        }
        else
        {
            unsigned char decode_stream = 1;

            switch (uws_client->uws_state)
            {
            default:
            case UWS_STATE_CLOSED:
                decode_stream = 0;
                break;

            case UWS_STATE_OPENING_UNDERLYING_IO:
                /* Codes_SRS_UWS_CLIENT_01_417: [ When `on_underlying_io_bytes_received` is called while OPENING but before the `on_underlying_io_open_complete` has been called, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_BYTES_RECEIVED_BEFORE_UNDERLYING_OPEN`. ]*/
                indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_BYTES_RECEIVED_BEFORE_UNDERLYING_OPEN);
                decode_stream = 0;
                break;

            case UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE:
            {
                /* Codes_SRS_UWS_CLIENT_01_378: [ When `on_underlying_io_bytes_received` is called while the uws is OPENING, the received bytes shall be accumulated in order to attempt parsing the WebSocket Upgrade response. ]*/
                unsigned char* new_received_bytes = (unsigned char*)realloc(uws_client->received_bytes, uws_client->received_bytes_count + size + 1);
                if (new_received_bytes == NULL)
                {
                    /* Codes_SRS_UWS_CLIENT_01_379: [ If allocating memory for accumulating the bytes fails, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_NOT_ENOUGH_MEMORY`. ]*/
                    indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_NOT_ENOUGH_MEMORY);
                    decode_stream = 0;
                }
                else
                {
                    uws_client->received_bytes = new_received_bytes;
                    (void)memcpy(uws_client->received_bytes + uws_client->received_bytes_count, buffer, size);
                    uws_client->received_bytes_count += size;

                    decode_stream = 1;
                }

                break;
            }

            case UWS_STATE_OPEN:
            {
                /* Codes_SRS_UWS_CLIENT_01_385: [ If the state of the uws instance is OPEN, the received bytes shall be used for decoding WebSocket frames. ]*/
                unsigned char* new_received_bytes = (unsigned char*)realloc(uws_client->received_bytes, uws_client->received_bytes_count + size + 1);
                if (new_received_bytes == NULL)
                {
                    /* Codes_SRS_UWS_CLIENT_01_418: [ If allocating memory for the bytes accumulated for decoding WebSocket frames fails, an error shall be indicated by calling the `on_ws_error` callback with `WS_ERROR_NOT_ENOUGH_MEMORY`. ]*/
                    LogError("Cannot allocate memory for received data");
                    indicate_ws_error(uws_client, WS_ERROR_NOT_ENOUGH_MEMORY);

                    decode_stream = 0;
                }
                else
                {
                    uws_client->received_bytes = new_received_bytes;
                    (void)memcpy(uws_client->received_bytes + uws_client->received_bytes_count, buffer, size);
                    uws_client->received_bytes_count += size;
                
                    decode_stream = 1;
                }

                break;
            }
            }

            while (decode_stream)
            {
                decode_stream = 0;

                switch (uws_client->uws_state)
                {
                default:
                case UWS_STATE_CLOSED:
                    break;

                case UWS_STATE_OPENING_UNDERLYING_IO:
                    /* Codes_SRS_UWS_CLIENT_01_417: [ When `on_underlying_io_bytes_received` is called while OPENING but before the `on_underlying_io_open_complete` has been called, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_BYTES_RECEIVED_BEFORE_UNDERLYING_OPEN`. ]*/
                    indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_BYTES_RECEIVED_BEFORE_UNDERLYING_OPEN);
                    break;

                case UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE:
                {
                    char* request_end_ptr;

                    /* Make sure it is zero terminated */
                    uws_client->received_bytes[uws_client->received_bytes_count] = '\0';

                    /* Codes_SRS_UWS_CLIENT_01_380: [ If an WebSocket Upgrade request can be parsed from the accumulated bytes, the status shall be read from the WebSocket upgrade response. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_381: [ If the status is 101, uws shall be considered OPEN and this shall be indicated by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `IO_OPEN_OK`. ]*/
                    if ((uws_client->received_bytes_count >= 4) &&
                        ((request_end_ptr = strstr((const char*)uws_client->received_bytes, "\r\n\r\n")) != NULL))
                    {
                        int status_code;

                        /* This part should really be done with the HTTPAPI, but that has to be done as a separate step
                        as the HTTPAPI has to expose somehow the underlying IO and currently this would be a too big of a change. */

                        /* Codes_SRS_UWS_CLIENT_01_382: [ If a negative status is decoded from the WebSocket upgrade request, an error shall be indicated by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_BAD_RESPONSE_STATUS`. ]*/
                        /* Codes_SRS_UWS_CLIENT_01_478: [ A Status-Line with a 101 response code as per RFC 2616 [RFC2616]. ]*/
                        if (ParseHttpResponse((const char*)uws_client->received_bytes, &status_code) != 0)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_383: [ If the WebSocket upgrade request cannot be decoded an error shall be indicated by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_BAD_UPGRADE_RESPONSE`. ]*/
                            LogError("Cannot decode HTTP response");
                            indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_BAD_UPGRADE_RESPONSE);
                        }
                        else if (status_code != 101)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_382: [ If a negative status is decoded from the WebSocket upgrade request, an error shall be indicated by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_BAD_RESPONSE_STATUS`. ]*/
                            LogError("Bad status (%d) received in WebSocket Upgrade response", status_code);
                            indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_BAD_RESPONSE_STATUS);
                        }
                        else
                        {
                            /* Codes_SRS_UWS_CLIENT_01_384: [ Any extra bytes that are left unconsumed after decoding a succesfull WebSocket upgrade response shall be used for decoding WebSocket frames ]*/
                            consume_received_bytes(uws_client, request_end_ptr - (char*)uws_client->received_bytes + 4);

                            /* Codes_SRS_UWS_CLIENT_01_381: [ If the status is 101, uws shall be considered OPEN and this shall be indicated by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `IO_OPEN_OK`. ]*/
                            uws_client->uws_state = UWS_STATE_OPEN;

                            /* Codes_SRS_UWS_CLIENT_01_065: [ When the client is to _Establish a WebSocket Connection_ given a set of (/host/, /port/, /resource name/, and /secure/ flag), along with a list of /protocols/ and /extensions/ to be used, and an /origin/ in the case of web browsers, it MUST open a connection, send an opening handshake, and read the server's handshake in response. ]*/
                            /* Codes_SRS_UWS_CLIENT_01_115: [ If the server's response is validated as provided for above, it is said that _The WebSocket Connection is Established_ and that the WebSocket Connection is in the OPEN state. ]*/
                            uws_client->on_ws_open_complete(uws_client->on_ws_open_complete_context, WS_OPEN_OK);

                            decode_stream = 1;
                        }
                    }

                    break;
                }

                case UWS_STATE_OPEN:
                {
                    size_t needed_bytes = 2;
                    size_t length;

                    /* Codes_SRS_UWS_CLIENT_01_277: [ To receive WebSocket data, an endpoint listens on the underlying network connection. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_278: [ Incoming data MUST be parsed as WebSocket frames as defined in Section 5.2. ]*/
                    if (uws_client->received_bytes_count >= needed_bytes)
                    {
                        unsigned char has_error = 0;

                        /* Codes_SRS_UWS_CLIENT_01_160: [ Defines whether the "Payload data" is masked. ]*/
                        if ((uws_client->received_bytes[1] & 0x80) != 0)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_144: [ A client MUST close a connection if it detects a masked frame. ]*/
                            /* Codes_SRS_UWS_CLIENT_01_145: [ In this case, it MAY use the status code 1002 (protocol error) as defined in Section 7.4.1. (These rules might be relaxed in a future specification.) ]*/
                            indicate_ws_error_and_close(uws_client, WS_ERROR_BAD_FRAME_RECEIVED, 1002);
                        }

                        /* Codes_SRS_UWS_CLIENT_01_163: [ The length of the "Payload data", in bytes: ]*/
                        /* Codes_SRS_UWS_CLIENT_01_164: [ if 0-125, that is the payload length. ]*/
                        length = uws_client->received_bytes[1];

                        if (length == 126)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_165: [ If 126, the following 2 bytes interpreted as a 16-bit unsigned integer are the payload length. ]*/
                            needed_bytes += 2;
                            if (uws_client->received_bytes_count >= needed_bytes)
                            {
                                /* Codes_SRS_UWS_CLIENT_01_167: [ Multibyte length quantities are expressed in network byte order. ]*/
                                length = ((uint64_t)(uws_client->received_bytes[2]) << 8) + uws_client->received_bytes[3];

                                if (length < 126)
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_168: [ Note that in all cases, the minimal number of bytes MUST be used to encode the length, for example, the length of a 124-byte-long string can't be encoded as the sequence 126, 0, 124. ]*/
                                    LogError("Bad frame: received a %u length on the 16 bit length", (unsigned int)length);

                                    /* Codes_SRS_UWS_CLIENT_01_419: [ If there is an error decoding the WebSocket frame, an error shall be indicated by calling the `on_ws_error` callback with `WS_ERROR_BAD_FRAME_RECEIVED`. ]*/
                                    indicate_ws_error(uws_client, WS_ERROR_BAD_FRAME_RECEIVED);
                                    has_error = 1;
                                }
                                else
                                {
                                    needed_bytes += (size_t)length;
                                }
                            }
                        }
                        else if (length == 127)
                        {
                            /* Codes_SRS_UWS_CLIENT_01_166: [ If 127, the following 8 bytes interpreted as a 64-bit unsigned integer (the most significant bit MUST be 0) are the payload length. ]*/
                            needed_bytes += 8;
                            if (uws_client->received_bytes_count >= needed_bytes)
                            {
                                if ((uws_client->received_bytes[2] & 0x80) != 0)
                                {
                                    LogError("Bad frame: received a 64 bit length frame with the highest bit set");

                                    /* Codes_SRS_UWS_CLIENT_01_419: [ If there is an error decoding the WebSocket frame, an error shall be indicated by calling the `on_ws_error` callback with `WS_ERROR_BAD_FRAME_RECEIVED`. ]*/
                                    indicate_ws_error(uws_client, WS_ERROR_BAD_FRAME_RECEIVED);
                                    has_error = 1;
                                }
                                else
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_167: [ Multibyte length quantities are expressed in network byte order. ]*/
                                    length = (size_t)(((uint64_t)(uws_client->received_bytes[2]) << 56) +
                                        (((uint64_t)uws_client->received_bytes[3]) << 48) +
                                        (((uint64_t)uws_client->received_bytes[4]) << 40) +
                                        (((uint64_t)uws_client->received_bytes[5]) << 32) +
                                        (((uint64_t)uws_client->received_bytes[6]) << 24) +
                                        (((uint64_t)uws_client->received_bytes[7]) << 16) +
                                        (((uint64_t)uws_client->received_bytes[8]) << 8) +
                                        (uint64_t)(uws_client->received_bytes[9]));

                                    if (length < 65536)
                                    {
                                        /* Codes_SRS_UWS_CLIENT_01_168: [ Note that in all cases, the minimal number of bytes MUST be used to encode the length, for example, the length of a 124-byte-long string can't be encoded as the sequence 126, 0, 124. ]*/
                                        LogError("Bad frame: received a %u length on the 64 bit length", (unsigned int)length);

                                        /* Codes_SRS_UWS_CLIENT_01_419: [ If there is an error decoding the WebSocket frame, an error shall be indicated by calling the `on_ws_error` callback with `WS_ERROR_BAD_FRAME_RECEIVED`. ]*/
                                        indicate_ws_error(uws_client, WS_ERROR_BAD_FRAME_RECEIVED);
                                        has_error = 1;
                                    }
                                    else
                                    {
                                        needed_bytes += length;
                                    }
                                }
                            }
                        }
                        else
                        {
                            needed_bytes += length;
                        }

                        if ((has_error == 0) &&
                            (uws_client->received_bytes_count >= needed_bytes))
                        {
                            unsigned char opcode = uws_client->received_bytes[0] & 0xF;

                            switch (opcode)
                            {
                            default:
                                break;

                                /* Codes_SRS_UWS_CLIENT_01_153: [ *  %x1 denotes a text frame ]*/
                                /* Codes_SRS_UWS_CLIENT_01_258: [** Currently defined opcodes for data frames include 0x1 (Text), 0x2 (Binary). ]*/
                            case (unsigned char)WS_TEXT_FRAME:
                                /* Codes_SRS_UWS_CLIENT_01_386: [ When a WebSocket data frame is decoded succesfully it shall be indicated via the callback `on_ws_frame_received`. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_169: [ The payload length is the length of the "Extension data" + the length of the "Application data". ]*/
                                /* Codes_SRS_UWS_CLIENT_01_173: [ The "Payload data" is defined as "Extension data" concatenated with "Application data". ]*/
                                /* Codes_SRS_UWS_CLIENT_01_280: [ Upon receiving a data frame (Section 5.6), the endpoint MUST note the /type/ of the data as defined by the opcode (frame-opcode) from Section 5.2. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_281: [ The "Application data" from this frame is defined as the /data/ of the message. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_282: [ If the frame comprises an unfragmented message (Section 5.4), it is said that _A WebSocket Message Has Been Received_ with type /type/ and data /data/. ]*/
                                uws_client->on_ws_frame_received(uws_client->on_ws_frame_received_context, WS_FRAME_TYPE_TEXT, uws_client->received_bytes + needed_bytes - length, length);
                                decode_stream = 1;
                                break;

                                /* Codes_SRS_UWS_CLIENT_01_154: [ *  %x2 denotes a binary frame ]*/
                            case (unsigned char)WS_BINARY_FRAME:
                                /* Codes_SRS_UWS_CLIENT_01_386: [ When a WebSocket data frame is decoded succesfully it shall be indicated via the callback `on_ws_frame_received`. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_169: [ The payload length is the length of the "Extension data" + the length of the "Application data". ]*/
                                /* Codes_SRS_UWS_CLIENT_01_173: [ The "Payload data" is defined as "Extension data" concatenated with "Application data". ]*/
                                /* Codes_SRS_UWS_CLIENT_01_264: [ The "Payload data" is arbitrary binary data whose interpretation is solely up to the application layer. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_280: [ Upon receiving a data frame (Section 5.6), the endpoint MUST note the /type/ of the data as defined by the opcode (frame-opcode) from Section 5.2. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_281: [ The "Application data" from this frame is defined as the /data/ of the message. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_282: [ If the frame comprises an unfragmented message (Section 5.4), it is said that _A WebSocket Message Has Been Received_ with type /type/ and data /data/. ]*/
                                uws_client->on_ws_frame_received(uws_client->on_ws_frame_received_context, WS_FRAME_TYPE_BINARY, uws_client->received_bytes + needed_bytes - length, length);
                                decode_stream = 1;
                                break;

                                /* Codes_SRS_UWS_CLIENT_01_156: [ *  %x8 denotes a connection close ]*/
                                /* Codes_SRS_UWS_CLIENT_01_234: [ The Close frame contains an opcode of 0x8. ]*/
                            case (unsigned char)WS_CLOSE_FRAME:
                            {
                                uint16_t close_code;
                                uint16_t* close_code_ptr;
                                const unsigned char* data_ptr = uws_client->received_bytes + needed_bytes - length;
                                const unsigned char* extra_data_ptr;
                                size_t extra_data_length;
                                unsigned char* close_frame_bytes;
                                size_t close_frame_length;
                                bool utf8_error = false;

                                /* Codes_SRS_UWS_CLIENT_01_235: [ The Close frame MAY contain a body (the "Application data" portion of the frame) that indicates a reason for closing, such as an endpoint shutting down, an endpoint having received a frame too large, or an endpoint having received a frame that does not conform to the format expected by the endpoint. ]*/
                                if (length >= 2)
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_236: [ If there is a body, the first two bytes of the body MUST be a 2-byte unsigned integer (in network byte order) representing a status code with value /code/ defined in Section 7.4. ]*/
                                    close_code = (data_ptr[0] << 8) + data_ptr[1];

                                    /* Codes_SRS_UWS_CLIENT_01_461: [ The argument `close_code` shall be set to point to the code extracted from the CLOSE frame. ]*/
                                    close_code_ptr = &close_code;
                                }
                                else
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_462: [ If no code can be extracted then `close_code` shall be NULL. ]*/
                                    close_code_ptr = NULL;
                                }

                                if (length > 2)
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_463: [ The extra bytes (besides the close code) shall be passed to the `on_ws_peer_closed` callback by using `extra_data` and `extra_data_length`. ]*/
                                    extra_data_ptr = data_ptr + 2;
                                    extra_data_length = length - 2;

                                    /* Codes_SRS_UWS_CLIENT_01_238: [ As the data is not guaranteed to be human readable, clients MUST NOT show it to end users. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_237: [ Following the 2-byte integer, the body MAY contain UTF-8-encoded data with value /reason/, the interpretation of which is not defined by this specification. ]*/
                                    if (utf8_checker_is_valid_utf8(extra_data_ptr, extra_data_length) != true)
                                    {
                                        LogError("Reason in CLOSE frame is not UTF-8.");
                                        extra_data_ptr = NULL;
                                        extra_data_length = 0;
                                        utf8_error = true;
                                    }
                                }
                                else
                                {
                                    extra_data_ptr = NULL;
                                    extra_data_length = 0;
                                }

                                if (utf8_error)
                                {
                                    uws_client->uws_state = UWS_STATE_CLOSING_UNDERLYING_IO;
                                    if (xio_close(uws_client->underlying_io, on_underlying_io_close_complete, uws_client) != 0)
                                    {
                                        indicate_ws_error(uws_client, WS_ERROR_CANNOT_CLOSE_UNDERLYING_IO);
                                        uws_client->uws_state = UWS_STATE_CLOSED;
                                    }
                                }
                                else
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_296: [ Upon either sending or receiving a Close control frame, it is said that _The WebSocket Closing Handshake is Started_ and that the WebSocket connection is in the CLOSING state. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_240: [ The application MUST NOT send any more data frames after sending a Close frame. ]*/
                                    uws_client->uws_state = UWS_STATE_CLOSING_SENDING_CLOSE;

                                    /* Codes_SRS_UWS_CLIENT_01_241: [ If an endpoint receives a Close frame and did not previously send a Close frame, the endpoint MUST send a Close frame in response. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_242: [ It SHOULD do so as soon as practical. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_239: [ Close frames sent from client to server must be masked as per Section 5.3. ]*/
                                    /* Codes_SRS_UWS_CLIENT_01_140: [ To avoid confusing network intermediaries (such as intercepting proxies) and for security reasons that are further discussed in Section 10.3, a client MUST mask all frames that it sends to the server (see Section 5.3 for further details). ]*/
                                    if (uws_frame_encoder_encode(uws_client->encode_buffer, WS_CLOSE_FRAME, NULL, 0, true, true, 0) != 0)
                                    {
                                        LogError("Cannot encode the response CLOSE frame");

                                        /* Codes_SRS_UWS_CLIENT_01_288: [ To _Close the WebSocket Connection_, an endpoint closes the underlying TCP connection. ]*/
                                        /* Codes_SRS_UWS_CLIENT_01_290: [ An endpoint MAY close the connection via any means available when necessary, such as when under attack. ]*/
                                        uws_client->uws_state = UWS_STATE_CLOSING_UNDERLYING_IO;
                                        if (xio_close(uws_client->underlying_io, on_underlying_io_close_complete, uws_client) != 0)
                                        {
                                            indicate_ws_error(uws_client, WS_ERROR_CANNOT_CLOSE_UNDERLYING_IO);
                                            uws_client->uws_state = UWS_STATE_CLOSED;
                                        }
                                    }
                                    else
                                    {
                                        close_frame_bytes = BUFFER_u_char(uws_client->encode_buffer);
                                        close_frame_length = BUFFER_length(uws_client->encode_buffer);
                                        if (xio_send(uws_client->underlying_io, close_frame_bytes, close_frame_length, on_underlying_io_close_sent, uws_client) != 0)
                                        {
                                            LogError("Cannot send the response CLOSE frame");

                                            /* Codes_SRS_UWS_CLIENT_01_288: [ To _Close the WebSocket Connection_, an endpoint closes the underlying TCP connection. ]*/
                                            /* Codes_SRS_UWS_CLIENT_01_290: [ An endpoint MAY close the connection via any means available when necessary, such as when under attack. ]*/
                                            uws_client->uws_state = UWS_STATE_CLOSING_UNDERLYING_IO;
                                            if (xio_close(uws_client->underlying_io, on_underlying_io_close_complete, uws_client) != 0)
                                            {
                                                indicate_ws_error(uws_client, WS_ERROR_CANNOT_CLOSE_UNDERLYING_IO);
                                                uws_client->uws_state = UWS_STATE_CLOSED;
                                            }
                                        }
                                    }
                                }

                                /* Codes_SRS_UWS_CLIENT_01_460: [ When a CLOSE frame is received the callback `on_ws_peer_closed` passed to `uws_client_open` shall be called, while passing to it the argument `on_ws_peer_closed_context`. ]*/
                                uws_client->on_ws_peer_closed(uws_client->on_ws_peer_closed_context, close_code_ptr, extra_data_ptr, extra_data_length);

                                break;
                            }

                                /* Codes_SRS_UWS_CLIENT_01_157: [ *  %x9 denotes a ping ]*/
                                /* Codes_SRS_UWS_CLIENT_01_247: [ The Ping frame contains an opcode of 0x9. ]*/
                                /* Codes_SRS_UWS_CLIENT_01_251: [ An endpoint MAY send a Ping frame any time after the connection is established and before the connection is closed. ]*/
                            case (unsigned char)WS_PING_FRAME:
                            {
                                /* Codes_SRS_UWS_CLIENT_01_249: [ Upon receipt of a Ping frame, an endpoint MUST send a Pong frame in response ]*/
                                /* Codes_SRS_UWS_CLIENT_01_250: [ It SHOULD respond with Pong frame as soon as is practical. ]*/
                                unsigned char* pong_frame;
                                size_t pong_frame_length;

                                uws_client->uws_state = UWS_STATE_ERROR;

                                /* Codes_SRS_UWS_CLIENT_01_140: [ To avoid confusing network intermediaries (such as intercepting proxies) and for security reasons that are further discussed in Section 10.3, a client MUST mask all frames that it sends to the server (see Section 5.3 for further details). ]*/
                                if (uws_frame_encoder_encode(uws_client->encode_buffer, WS_PONG_FRAME, uws_client->received_bytes + needed_bytes - length, length, true, true, 0) != 0)
                                {
                                    LogError("Encoding of PONG failed.");
                                }
                                else
                                {
                                    /* Codes_SRS_UWS_CLIENT_01_248: [ A Ping frame MAY include "Application data". ]*/
                                    pong_frame = BUFFER_u_char(uws_client->encode_buffer);
                                    pong_frame_length = BUFFER_length(uws_client->encode_buffer);
                                    if (xio_send(uws_client->underlying_io, pong_frame, pong_frame_length, NULL, NULL) != 0)
                                    {
                                        LogError("Sending CLOSE frame failed.");
                                    }
                                }

                                break;
                            }
                            /* Codes_SRS_UWS_CLIENT_01_252: [ The Pong frame contains an opcode of 0xA. ]*/
                            case (unsigned char)WS_PONG_FRAME:
                                break;
                            }

                            consume_received_bytes(uws_client, needed_bytes);
                        }
                    }

                    break;
                }
                }
            }
        }
    }
}

static void on_underlying_io_error(void* context)
{
    UWS_CLIENT_HANDLE uws_client = (UWS_CLIENT_HANDLE)context;

    switch (uws_client->uws_state)
    {
    default:
        break;

    case UWS_STATE_CLOSING_UNDERLYING_IO:
    case UWS_STATE_CLOSING_SENDING_CLOSE:
        /* Codes_SRS_UWS_CLIENT_01_377: [ When `on_underlying_io_error` is called while the uws instance is CLOSING shall indicate the error by calling the `on_ws_close_complete` callback and then it shall set the uws client in the CLOSED state. ]*/
        indicate_ws_error(uws_client, WS_ERROR_UNDERLYING_IO_ERROR);
        xio_close(uws_client->underlying_io, NULL, NULL);
        uws_client->uws_state = UWS_STATE_CLOSED;
        break;

    case UWS_STATE_OPENING_UNDERLYING_IO:
    case UWS_STATE_WAITING_FOR_UPGRADE_RESPONSE:
        /* Codes_SRS_UWS_CLIENT_01_375: [ When `on_underlying_io_error` is called while uws is OPENING, uws shall report that the open failed by calling the `on_ws_open_complete` callback passed to `uws_client_open` with `WS_OPEN_ERROR_UNDERLYING_IO_ERROR`. ]*/
        /* Codes_SRS_UWS_CLIENT_01_077: [ If this fails (e.g., the server's certificate could not be verified), then the client MUST _Fail the WebSocket Connection_ and abort the connection. ]*/
        indicate_ws_open_complete_error_and_close(uws_client, WS_OPEN_ERROR_UNDERLYING_IO_ERROR);
        break;

    case UWS_STATE_OPEN:
        /* Codes_SRS_UWS_CLIENT_01_376: [ When `on_underlying_io_error` is called while the uws instance is OPEN, an error shall be reported to the user by calling the `on_ws_error` callback that was passed to `uws_client_open` with the argument `WS_ERROR_UNDERLYING_IO_ERROR`. ]*/
        /* Codes_SRS_UWS_CLIENT_01_318: [ Servers MAY close the WebSocket connection whenever desired. ]*/
        /* Codes_SRS_UWS_CLIENT_01_269: [ If at any point the state of the WebSocket connection changes, the endpoint MUST abort the following steps. ]*/
        indicate_ws_error(uws_client, WS_ERROR_UNDERLYING_IO_ERROR);
        break;
    }
}

int uws_client_open(UWS_CLIENT_HANDLE uws_client, ON_WS_OPEN_COMPLETE on_ws_open_complete, void* on_ws_open_complete_context, ON_WS_FRAME_RECEIVED on_ws_frame_received, void* on_ws_frame_received_context, ON_WS_PEER_CLOSED on_ws_peer_closed, void* on_ws_peer_closed_context, ON_WS_ERROR on_ws_error, void* on_ws_error_context)
{
    int result;

    /* Codes_SRS_UWS_CLIENT_01_393: [ The context arguments for the callbacks shall be allowed to be NULL. ]*/
    if ((uws_client == NULL) ||
        (on_ws_open_complete == NULL) ||
        (on_ws_frame_received == NULL) ||
        (on_ws_peer_closed == NULL) ||
        (on_ws_error == NULL))
    {
        /* Codes_SRS_UWS_CLIENT_01_027: [ If `uws_client`, `on_ws_open_complete`, `on_ws_frame_received`, `on_ws_peer_closed` or `on_ws_error` is NULL, `uws_client_open` shall fail and return a non-zero value. ]*/
        LogError("Invalid arguments: uws=%p, on_ws_open_complete=%p, on_ws_frame_received=%p, on_ws_error=%p",
            uws_client, on_ws_open_complete, on_ws_frame_received, on_ws_error);
        result = __LINE__;
    }
    else
    {
        if (uws_client->uws_state != UWS_STATE_CLOSED)
        {
            /* Codes_SRS_UWS_CLIENT_01_400: [ `uws_client_open` while CLOSING shall fail and return a non-zero value. ]*/
            /* Codes_SRS_UWS_CLIENT_01_394: [ `uws_client_open` while the uws instance is already OPEN or OPENING shall fail and return a non-zero value. ]*/
            LogError("Invalid uWS state while trying to open: %d", (int)uws_client->uws_state);
            result = __LINE__;
        }
        else
        {
            uws_client->uws_state = UWS_STATE_OPENING_UNDERLYING_IO;

            /* Codes_SRS_UWS_CLIENT_01_025: [ `uws_client_open` shall open the underlying IO by calling `xio_open` and providing the IO handle created in `uws_client_create` as argument. ]*/
            /* Codes_SRS_UWS_CLIENT_01_367: [ The callbacks `on_underlying_io_open_complete`, `on_underlying_io_bytes_received` and `on_underlying_io_error` shall be passed as arguments to `xio_open`. ]*/
            /* Codes_SRS_UWS_CLIENT_01_061: [ To _Establish a WebSocket Connection_, a client opens a connection and sends a handshake as defined in this section. ]*/
            if (xio_open(uws_client->underlying_io, on_underlying_io_open_complete, uws_client, on_underlying_io_bytes_received, uws_client, on_underlying_io_error, uws_client) != 0)
            {
                /* Codes_SRS_UWS_CLIENT_01_028: [ If opening the underlying IO fails then `uws_client_open` shall fail and return a non-zero value. ]*/
                /* Codes_SRS_UWS_CLIENT_01_075: [ If the connection could not be opened, either because a direct connection failed or because any proxy used returned an error, then the client MUST _Fail the WebSocket Connection_ and abort the connection attempt. ]*/
                LogError("Opening the underlying IO failed");
                uws_client->uws_state = UWS_STATE_CLOSED;
                result = __LINE__;
            }
            else
            {
                uws_client->received_bytes_count = 0;

                uws_client->on_ws_open_complete = on_ws_open_complete;
                uws_client->on_ws_open_complete_context = on_ws_open_complete_context;
                uws_client->on_ws_frame_received = on_ws_frame_received;
                uws_client->on_ws_frame_received_context = on_ws_frame_received_context;
                uws_client->on_ws_peer_closed = on_ws_peer_closed;
                uws_client->on_ws_peer_closed_context = on_ws_peer_closed_context;
                uws_client->on_ws_error = on_ws_error;
                uws_client->on_ws_error_context = on_ws_error_context;

                /* Codes_SRS_UWS_CLIENT_01_026: [ On success, `uws_client_open` shall return 0. ]*/
                result = 0;
            }
        }
    }

    return result;
}

static int complete_send_frame(WS_PENDING_SEND* ws_pending_send, LIST_ITEM_HANDLE pending_send_frame_item, WS_SEND_FRAME_RESULT ws_send_frame_result)
{
    int result;
    UWS_CLIENT_INSTANCE* uws_client = ws_pending_send->uws_client;

    /* Codes_SRS_UWS_CLIENT_01_432: [ The indicated sent frame shall be removed from the list by calling `singlylinkedlist_remove`. ]*/
    if (singlylinkedlist_remove(uws_client->pending_sends, pending_send_frame_item) != 0)
    {
        result = __LINE__;
    }
    else
    {
        if (ws_pending_send->on_ws_send_frame_complete != NULL)
        {
            /* Codes_SRS_UWS_CLIENT_01_037: [ When indicating pending send frames as cancelled the callback context passed to the `on_ws_send_frame_complete` callback shall be the context given to `uws_client_send_frame`. ]*/
            ws_pending_send->on_ws_send_frame_complete(ws_pending_send->context, ws_send_frame_result);
        }

        /* Codes_SRS_UWS_CLIENT_01_434: [ The memory associated with the sent frame shall be freed. ]*/
        free(ws_pending_send);

        result = 0;
    }

    return result;
}

/* Codes_SRS_UWS_CLIENT_01_029: [ `uws_client_close` shall close the uws instance connection if an open action is either pending or has completed successfully (if the IO is open). ]*/
/* Codes_SRS_UWS_CLIENT_01_317: [ Clients SHOULD NOT close the WebSocket connection arbitrarily. ]*/
int uws_client_close(UWS_CLIENT_HANDLE uws_client, ON_WS_CLOSE_COMPLETE on_ws_close_complete, void* on_ws_close_complete_context)
{
    int result;

    /* Codes_SRS_UWS_CLIENT_01_397: [ The `on_ws_close_complete` argument shall be allowed to be NULL, in which case no callback shall be called when the close is complete. ]*/
    /* Codes_SRS_UWS_CLIENT_01_398: [ `on_ws_close_complete_context` shall also be allows to be NULL. ]*/
    if (uws_client == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_030: [ if `uws_client` is NULL, `uws_client_close` shall return a non-zero value. ]*/
        LogError("NULL uWS handle.");
        result = __LINE__;
    }
    else
    {
        if ((uws_client->uws_state == UWS_STATE_CLOSED) ||
            (uws_client->uws_state == UWS_STATE_CLOSING_SENDING_CLOSE) ||
            (uws_client->uws_state == UWS_STATE_CLOSING_UNDERLYING_IO))
        {
            /* Codes_SRS_UWS_CLIENT_01_032: [ `uws_client_close` when no open action has been issued shall fail and return a non-zero value. ]*/
            LogError("close has been called when already CLOSED");
            result = __LINE__;
        }
        else
        {
            /* Codes_SRS_UWS_CLIENT_01_399: [ `on_ws_close_complete` and `on_ws_close_complete_context` shall be saved and the callback `on_ws_close_complete` shall be triggered when the close is complete. ]*/
            uws_client->on_ws_close_complete = on_ws_close_complete;
            uws_client->on_ws_close_complete_context = on_ws_close_complete_context;

            uws_client->uws_state = UWS_STATE_CLOSING_UNDERLYING_IO;

            /* Codes_SRS_UWS_CLIENT_01_031: [ `uws_client_close` shall close the connection by calling `xio_close` while passing as argument the IO handle created in `uws_client_create`. ]*/
            /* Codes_SRS_UWS_CLIENT_01_368: [ The callback `on_underlying_io_close` shall be passed as argument to `xio_close`. ]*/
            if (xio_close(uws_client->underlying_io, (on_ws_close_complete == NULL) ? NULL :  on_underlying_io_close_complete, (on_ws_close_complete == NULL) ? NULL : uws_client) != 0)
            {
                /* Codes_SRS_UWS_CLIENT_01_395: [ If `xio_close` fails, `uws_client_close` shall fail and return a non-zero value. ]*/
                LogError("Closing the underlying IO failed.");
                result = __LINE__;
            }
            else
            {
                /* Codes_SRS_UWS_CLIENT_01_034: [ `uws_client_close` shall obtain all the pending send frames by repetitively querying for the head of the pending IO list and freeing that head item. ]*/
                LIST_ITEM_HANDLE first_pending_send;

                /* Codes_SRS_UWS_CLIENT_01_035: [ Obtaining the head of the pending send frames list shall be done by calling `singlylinkedlist_get_head_item`. ]*/
                while ((first_pending_send = singlylinkedlist_get_head_item(uws_client->pending_sends)) != NULL)
                {
                    WS_PENDING_SEND* ws_pending_send = (WS_PENDING_SEND*)singlylinkedlist_item_get_value(first_pending_send);

                    /* Codes_SRS_UWS_CLIENT_01_036: [ For each pending send frame the send complete callback shall be called with `UWS_SEND_FRAME_CANCELLED`. ]*/
                    complete_send_frame(ws_pending_send, first_pending_send, WS_SEND_FRAME_CANCELLED);
                }

                /* Codes_SRS_UWS_CLIENT_01_396: [ On success `uws_client_close` shall return 0. ]*/
                result = 0;
            }
        }
    }

    return result;
}

/* Codes_SRS_UWS_CLIENT_01_317: [ Clients SHOULD NOT close the WebSocket connection arbitrarily. ]*/
int uws_client_close_handshake(UWS_CLIENT_HANDLE uws_client, uint16_t close_code, const char* close_reason, ON_WS_CLOSE_COMPLETE on_ws_close_complete, void* on_ws_close_complete_context)
{
    int result;

    if (uws_client == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_467: [ if `uws_client` is NULL, `uws_client_close_handshake` shall return a non-zero value. ]*/
        LogError("NULL uws_client");
        result = __LINE__;
    }
    else
    {
        if ((uws_client->uws_state == UWS_STATE_CLOSED) ||
            /* Codes_SRS_UWS_CLIENT_01_474: [ `uws_client_close_handshake` when already CLOSING shall fail and return a non-zero value. ]*/
            (uws_client->uws_state == UWS_STATE_CLOSING_SENDING_CLOSE) ||
            (uws_client->uws_state == UWS_STATE_CLOSING_UNDERLYING_IO))
        {
            /* Codes_SRS_UWS_CLIENT_01_473: [ `uws_client_close_handshake` when no open action has been issued shall fail and return a non-zero value. ]*/
            LogError("uws_client_close_handshake has been called when already CLOSED");
            result = __LINE__;
        }
        else
        {
            (void)close_reason;

            /* Codes_SRS_UWS_CLIENT_01_468: [ `on_ws_close_complete` and `on_ws_close_complete_context` shall be saved and the callback `on_ws_close_complete` shall be triggered when the close is complete. ]*/
            /* Codes_SRS_UWS_CLIENT_01_469: [ The `on_ws_close_complete` argument shall be allowed to be NULL, in which case no callback shall be called when the close is complete. ]*/
            /* Codes_SRS_UWS_CLIENT_01_470: [ `on_ws_close_complete_context` shall also be allowed to be NULL. ]*/
            uws_client->on_ws_close_complete = on_ws_close_complete;
            uws_client->on_ws_close_complete_context = on_ws_close_complete_context;

            /* Codes_SRS_UWS_CLIENT_01_465: [ `uws_client_close_handshake` shall initiate the close handshake by sending a close frame to the peer. ]*/
            if (send_close_frame(uws_client, close_code) != 0)
            {
                /* Codes_SRS_UWS_CLIENT_01_472: [ If `xio_send` fails, `uws_client_close_handshake` shall fail and return a non-zero value. ]*/
                LogError("Sending CLOSE frame failed");
                result = __LINE__;
            }
            else
            {
                LIST_ITEM_HANDLE first_pending_send;

                while ((first_pending_send = singlylinkedlist_get_head_item(uws_client->pending_sends)) != NULL)
                {
                    WS_PENDING_SEND* ws_pending_send = (WS_PENDING_SEND*)singlylinkedlist_item_get_value(first_pending_send);

                    complete_send_frame(ws_pending_send, first_pending_send, WS_SEND_FRAME_CANCELLED);
                }

                /* Codes_SRS_UWS_CLIENT_01_466: [ On success `uws_client_close_handshake` shall return 0. ]*/
                result = 0;
            }
        }
    }

    return result;
}

static void on_underlying_io_send_complete(void* context, IO_SEND_RESULT send_result)
{
    if (context == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_435: [ When `on_underlying_io_send_complete` is called with a NULL `context`, it shall do nothing. ]*/
        LogError("on_underlying_io_send_complete called with NULL context");
    }
    else
    {
        LIST_ITEM_HANDLE ws_pending_send_list_item = (LIST_ITEM_HANDLE)context;
        WS_PENDING_SEND* ws_pending_send = (WS_PENDING_SEND*)singlylinkedlist_item_get_value(ws_pending_send_list_item);
        UWS_CLIENT_HANDLE uws_client = ws_pending_send->uws_client;
        WS_SEND_FRAME_RESULT ws_send_frame_result;

        switch (send_result)
        {
        /* Codes_SRS_UWS_CLIENT_01_436: [ When `on_underlying_io_send_complete` is called with any other error code, the send shall be indicated to the uws user by calling `on_ws_send_frame_complete` with `WS_SEND_FRAME_ERROR`. ]*/
        default:
        case IO_SEND_ERROR:
            /* Codes_SRS_UWS_CLIENT_01_390: [ When `on_underlying_io_send_complete` is called with `IO_SEND_ERROR` as a result of sending a WebSocket frame to the underlying IO, the send shall be indicated to the uws user by calling `on_ws_send_frame_complete` with `WS_SEND_FRAME_ERROR`. ]*/
            ws_send_frame_result = WS_SEND_FRAME_ERROR;
            break;

        case IO_SEND_OK:
            /* Codes_SRS_UWS_CLIENT_01_389: [ When `on_underlying_io_send_complete` is called with `IO_SEND_OK` as a result of sending a WebSocket frame to the underlying IO, the send shall be indicated to the uws user by calling `on_ws_send_frame_complete` with `WS_SEND_FRAME_OK`. ]*/
            ws_send_frame_result = WS_SEND_FRAME_OK;
            break;

        case IO_SEND_CANCELLED:
            /* Codes_SRS_UWS_CLIENT_01_391: [ When `on_underlying_io_send_complete` is called with `IO_SEND_CANCELLED` as a result of sending a WebSocket frame to the underlying IO, the send shall be indicated to the uws user by calling `on_ws_send_frame_complete` with `WS_SEND_FRAME_CANCELLED`. ]*/
            ws_send_frame_result = WS_SEND_FRAME_CANCELLED;
            break;
        }

        if (complete_send_frame(ws_pending_send, ws_pending_send_list_item, ws_send_frame_result) != 0)
        {
            /* Codes_SRS_UWS_CLIENT_01_433: [ If `singlylinkedlist_remove` fails an error shall be indicated by calling the `on_ws_error` callback with `WS_ERROR_CANNOT_REMOVE_SENT_ITEM_FROM_LIST`. ]*/
            indicate_ws_error(uws_client, WS_ERROR_CANNOT_REMOVE_SENT_ITEM_FROM_LIST);
        }
    }
}

int uws_client_send_frame(UWS_CLIENT_HANDLE uws_client, unsigned char frame_type, const unsigned char* buffer, size_t size, bool is_final, ON_WS_SEND_FRAME_COMPLETE on_ws_send_frame_complete, void* on_ws_send_frame_complete_context)
{
    int result;

    (void)frame_type;

    if (uws_client == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_044: [ If any the arguments `uws_client` is NULL, `uws_client_send_frame` shall fail and return a non-zero value. ]*/
        LogError("NULL uws handle.");
        result = __LINE__;
    }
    else if ((buffer == NULL) &&
        (size > 0))
    {
        /* Codes_SRS_UWS_CLIENT_01_045: [ If `size` is non-zero and `buffer` is NULL then `uws_client_send_frame` shall fail and return a non-zero value. ]*/
        LogError("NULL buffer with %u size.", (unsigned int)size);
        result = __LINE__;
    }
    /* Codes_SRS_UWS_CLIENT_01_146: [ A data frame MAY be transmitted by either the client or the server at any time after opening handshake completion and before that endpoint has sent a Close frame (Section 5.5.1). ]*/
    /* Codes_SRS_UWS_CLIENT_01_268: [ The endpoint MUST ensure the WebSocket connection is in the OPEN state ]*/
    else if (uws_client->uws_state != UWS_STATE_OPEN)
    {
        /* Codes_SRS_UWS_CLIENT_01_043: [ If the uws instance is not OPEN (open has not been called or is still in progress) then `uws_client_send_frame` shall fail and return a non-zero value. ]*/
        LogError("uws not in OPEN state.");
        result = __LINE__;
    }
    else
    {
        WS_PENDING_SEND* ws_pending_send = (WS_PENDING_SEND*)malloc(sizeof(WS_PENDING_SEND));
        if (ws_pending_send == NULL)
        {
            /* Codes_SRS_UWS_CLIENT_01_047: [ If allocating memory for the newly queued item fails, `uws_client_send_frame` shall fail and return a non-zero value. ]*/
            LogError("Cannot allocate memory for frame to be sent.");
            result = __LINE__;
        }
        else
        {
            /* Codes_SRS_UWS_CLIENT_01_425: [ Encoding shall be done by calling `uws_frame_encoder_encode` and passing to it the `buffer` and `size` argument for payload, the `is_final` flag and setting `is_masked` to true. ]*/
            /* Codes_SRS_UWS_CLIENT_01_427: [ The encoded frame buffer that shall be used is the buffer created in `uws_client_create`. ]*/
            /* Codes_SRS_UWS_CLIENT_01_270: [ An endpoint MUST encapsulate the /data/ in a WebSocket frame as defined in Section 5.2. ]*/
            /* Codes_SRS_UWS_CLIENT_01_272: [ The opcode (frame-opcode) of the first frame containing the data MUST be set to the appropriate value from Section 5.2 for data that is to be interpreted by the recipient as text or binary data. ]*/
            /* Codes_SRS_UWS_CLIENT_01_274: [ If the data is being sent by the client, the frame(s) MUST be masked as defined in Section 5.3. ]*/
            if (uws_frame_encoder_encode(uws_client->encode_buffer, frame_type, buffer, size, true, is_final, 0) != 0)
            {
                /* Codes_SRS_UWS_CLIENT_01_426: [ If `uws_frame_encoder_encode` fails, `uws_client_send_frame` shall fail and return a non-zero value. ]*/
                free(ws_pending_send);
                result = __LINE__;
            }
            else
            {
                const unsigned char* encoded_frame;
                size_t encoded_frame_length;
                LIST_ITEM_HANDLE new_pending_send_list_item;

                /* Codes_SRS_UWS_CLIENT_01_428: [ The encoded frame buffer memory shall be obtained by calling `BUFFER_u_char` on the encode buffer. ]*/
                encoded_frame = BUFFER_u_char(uws_client->encode_buffer);
                /* Codes_SRS_UWS_CLIENT_01_429: [ The encoded frame size shall be obtained by calling `BUFFER_length` on the encode buffer. ]*/
                encoded_frame_length = BUFFER_length(uws_client->encode_buffer);

                /* Codes_SRS_UWS_CLIENT_01_038: [ `uws_client_send_frame` shall create and queue a structure that contains: ]*/
                /* Codes_SRS_UWS_CLIENT_01_050: [ The argument `on_ws_send_frame_complete` shall be optional, if NULL is passed by the caller then no send complete callback shall be triggered. ]*/
                /* Codes_SRS_UWS_CLIENT_01_040: [ - the send complete callback `on_ws_send_frame_complete` ]*/
                /* Codes_SRS_UWS_CLIENT_01_041: [ - the send complete callback context `on_ws_send_frame_complete_context` ]*/
                ws_pending_send->on_ws_send_frame_complete = on_ws_send_frame_complete;
                ws_pending_send->context = on_ws_send_frame_complete_context;
                ws_pending_send->uws_client = uws_client;

                /* Codes_SRS_UWS_CLIENT_01_048: [ Queueing shall be done by calling `singlylinkedlist_add`. ]*/
                new_pending_send_list_item = singlylinkedlist_add(uws_client->pending_sends, ws_pending_send);
                if (new_pending_send_list_item == NULL)
                {
                    /* Codes_SRS_UWS_CLIENT_01_049: [ If `singlylinkedlist_add` fails, `uws_client_send_frame` shall fail and return a non-zero value. ]*/
                    free(ws_pending_send);
                    result = __LINE__;
                }
                else
                {
                    /* Codes_SRS_UWS_CLIENT_01_431: [ Once encoded the frame shall be sent by using `xio_send` with the following arguments: ]*/
                    /* Codes_SRS_UWS_CLIENT_01_053: [ - the io handle shall be the underlyiong IO handle created in `uws_client_create`. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_054: [ - the `buffer` argument shall point to the complete websocket frame to be sent. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_055: [ - the `size` argument shall indicate the websocket frame length. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_056: [ - the `send_complete` callback shall be the `on_underlying_io_send_complete` function. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_057: [ - the `send_complete_context` argument shall identify the pending send. ]*/
                    /* Codes_SRS_UWS_CLIENT_01_276: [ The frame(s) that have been formed MUST be transmitted over the underlying network connection. ]*/
                    if (xio_send(uws_client->underlying_io, encoded_frame, encoded_frame_length, on_underlying_io_send_complete, new_pending_send_list_item) != 0)
                    {
                        /* Codes_SRS_UWS_CLIENT_01_058: [ If `xio_send` fails, `uws_client_send_frame` shall fail and return a non-zero value. ]*/
                        (void)singlylinkedlist_remove(uws_client->pending_sends, new_pending_send_list_item);
                        free(ws_pending_send);
                        result = __LINE__;
                    }
                    else
                    {
                        /* Codes_SRS_UWS_CLIENT_01_042: [ On success, `uws_client_send_frame` shall return 0. ]*/
                        result = 0;
                    }
                }
            }
        }
    }

    return result;
}

void uws_client_dowork(UWS_CLIENT_HANDLE uws_client)
{
    if (uws_client == NULL)
    {
        /* Codes_SRS_UWS_CLIENT_01_059: [ If the `uws_client` argument is NULL, `uws_client_dowork` shall do nothing. ]*/
        LogError("NULL uws handle.");
    }
    else
    {
        /* Codes_SRS_UWS_CLIENT_01_060: [ If the IO is not yet open, `uws_client_dowork` shall do nothing. ]*/
        if (uws_client->uws_state != UWS_STATE_CLOSED)
        {
            /* Codes_SRS_UWS_CLIENT_01_430: [ `uws_client_dowork` shall call `xio_dowork` with the IO handle argument set to the underlying IO created in `uws_client_create`. ]*/
            xio_dowork(uws_client->underlying_io);
        }
    }
}

int uws_client_set_option(UWS_CLIENT_HANDLE uws_client, const char* option_name, const void* value)
{
    int result;

    if (
        (uws_client == NULL) ||
        (option_name == NULL)
        )
    {
        /* Codes_SRS_UWS_CLIENT_01_440: [ If any of the arguments `uws_client` or `option_name` is NULL `uws_client_set_option` shall return a non-zero value. ]*/
        result = __LINE__;
        LogError("invalid parameter (NULL) passed to uws_client_set_option");
    }
    else
    {
        /* Codes_SRS_UWS_CLIENT_01_441: [ All options shall be passed as they are to the underlying IO by calling `xio_setoption`. ]*/
        if (xio_setoption(uws_client->underlying_io, option_name, value) != 0)
        {
            /* Codes_SRS_UWS_CLIENT_01_443: [ If `xio_setoption` fails, `uws_client_set_option` shall fail and return a non-zero value. ]*/
            LogError("xio_setoption failed.");
            result = __LINE__;
        }
        else
        {
            /* Codes_SRS_UWS_CLIENT_01_442: [ On success, `uws_client_set_option` shall return 0. ]*/
            result = 0;
        }
    }

    return result;
}

OPTIONHANDLER_HANDLE uws_client_retrieve_options(UWS_CLIENT_HANDLE uws_client)
{
    OPTIONHANDLER_HANDLE result;

    /* Codes_SRS_UWS_CLIENT_01_444: [ If parameter `uws_client` is `NULL` then `uws_client_retrieve_options` shall fail and return NULL. ]*/
    if (uws_client == NULL)
    {
        LogError("NULL uws handle.");
        result = NULL;
    }
    else
    {
        /* Codes_SRS_UWS_CLIENT_01_445: [ `uws_client_retrieve_options` shall call `xio_retrieveoptions` to produce an `OPTIONHANDLER_HANDLE` and on success return the new `OPTIONHANDLER_HANDLE` handle. ]*/
        result = xio_retrieveoptions(uws_client->underlying_io);
        if (result == NULL)
        {
            /* Codes_SRS_UWS_CLIENT_01_446: [ If `xio_retrieveoptions` fails then `uws_client_retrieve_options` shall fail and return NULL. ]*/
            LogError("xio_retrieveoptions failed.");
        }
    }

    return result;
}
