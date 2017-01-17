// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "azure_c_shared_utility/atrpc.h"

#ifdef __cplusplus
  #include <cassert>
  #include <cstdbool>
  #include <cstddef>
  #include <cstdlib>
#else
  #include <assert.h>
  #include <stdbool.h>
  #include <stddef.h>
  #include <stdlib.h>
#endif

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/uartio.h"
#include "azure_c_shared_utility/vector.h"
#include "azure_c_shared_utility/xlogging.h"

typedef enum ATRPC_STATUS_TAG
{
    ATRPC_CLOSED,
    ATRPC_HANDSHAKING,
    ATRPC_NEGOTIATING_AUTOBAUD,
    ATRPC_OPEN,
} ATRPC_STATUS;

typedef enum MODEM_IO_STATUS_TAG 
{
    MODEM_IO_CLOSED,
    MODEM_IO_OPEN,
    MODEM_IO_OPENING,
} MODEM_IO_STATUS;

typedef struct ATRPC_INSTANCE_TAG 
{
    bool awaiting_echo;
    tickcounter_ms_t call_origination_ms;
    unsigned char * current_request;
    size_t current_request_length;
    size_t echo_machine_state;
    size_t handshake_attempt;
    size_t handshake_machine_state;
    XIO_HANDLE modem_io;
    bool modem_receiving;
    MODEM_IO_STATUS modem_status;
    ON_OPEN_COMPLETE on_open_complete;
    void * on_open_complete_context;
    ON_TA_RESPONSE on_ta_response;
    void * on_ta_response_context;
    unsigned char * response_buffer;
    size_t ta_response_buffer_allocated_size;
    size_t ta_response_buffer_index;
    ATRPC_STATUS status;
    TA_RESULT_CODE ta_result_code;
    CUSTOM_TA_RESULT_CODE_PARSER ta_result_code_parser;
    void * ta_result_code_parser_context;
    size_t ta_result_code_machine_state;
    TICK_COUNTER_HANDLE tick_counter;
    size_t timeout_ms;
} ATRPC_INSTANCE;

static bool autobaud_negotiation_machine (ATRPC_HANDLE handle_, unsigned char byte_)
{
    bool response_complete;

    /* Codes_SRS_ATRPC_27_058: [ During auto-baud negotiation, modem_on_bytes_received() shall accept "0\r" as a valid response when on its own line. ] */
    /* Codes_SRS_ATRPC_27_059: [ During auto-baud negotiation, modem_on_bytes_received() shall accept "\r\nOK\r\n" as a valid response when on its own line. ] */
    switch (handle_->ta_result_code_machine_state)
    {
      case 0:
        response_complete = false;
        if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 1;
        }
        break;
      case 1:
        response_complete = false;
        if ('0' == byte_)
        {
            handle_->ta_result_code_machine_state = 2;
        }
        else if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 4;
        }
        else if ('\n' == byte_)
        {
            handle_->ta_result_code_machine_state = 3;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 2:
        response_complete = ('\r' == byte_);
        handle_->ta_result_code_machine_state = 0;
        break;
      case 3:
        response_complete = false;
        if ('0' == byte_)
        {
            handle_->ta_result_code_machine_state = 2;
        }
        else if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 4;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 4:
        response_complete = false;
        if ('0' == byte_)
        {
            handle_->ta_result_code_machine_state = 2;
        }
        else if ('\r' == byte_)
        {
            /* defer */
        }
        else if ('\n' == byte_)
        {
            handle_->ta_result_code_machine_state = 5;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 5:
        response_complete = false;
        if ('0' == byte_)
        {
            handle_->ta_result_code_machine_state = 2;
        }
        else if ('O' == byte_)
        {
            handle_->ta_result_code_machine_state = 6;
        }
        else if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 4;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 6:
        response_complete = false;
        if ('K' == byte_)
        {
            handle_->ta_result_code_machine_state = 7;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 7:
        response_complete = false;
        if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 8;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      case 8:
        response_complete = ('\n' == byte_);
        if ('0' == byte_)
        {
            handle_->ta_result_code_machine_state = 2;
        }
        else if ('\r' == byte_)
        {
            handle_->ta_result_code_machine_state = 4;
        }
        else
        {
            handle_->ta_result_code_machine_state = 0;
        }
        break;
      default:
        response_complete = false;
        break;
    }

    return response_complete;
}


static void clear_current_request (ATRPC_HANDLE handle_)
{
    free(handle_->current_request);
    handle_->current_request = NULL;
    handle_->current_request_length = 0;
    handle_->awaiting_echo = true;
    handle_->timeout_ms = 0;

    return;
}


static void modem_handshake (void * context_, TA_RESULT_CODE result_code_, const unsigned char * response_, size_t response_size_)
{
    (void)response_, response_size_;
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_handshake()!");
    }
    else
    {
        switch (result_code_)
        {
          case OK_3GPP:
            ++atrpc->handshake_machine_state;
            break;
          case CONNECT_3GPP:
          case RING_3GPP:
          case NO_CARRIER_3GPP:
          case ERROR_3GPP:
          case ERROR_ATRPC:
          case NO_DIALTONE_3GPP:
          case BUSY_3GPP:
          case NO_ANSWER_3GPP:
          case PROCEEDING_SIMCOM:
          default:
            ++atrpc->handshake_attempt;
            break;
        }

        if (50 < atrpc->handshake_attempt)
        {
            LogError("Failed to negotiate handshake before exhausting maximum allowed time-outs!");
            /* Codes_SRS_ATRPC_27_065: [ During auto-baud negotiation, if 50 or more time-outs occur, then modem_handshake() shall call the (void)on_open_complete(void * context, ta_result_code result_code, char * response) callback provided to atrpc_open(), using the on_open_complete_context argument provided to atrpc_open() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the response parameter. ] */
            atrpc->handshake_attempt = 0;
        	clear_current_request(atrpc);
            atrpc->on_open_complete(atrpc->on_open_complete_context, ERROR_ATRPC);
        }
        else
        {
            static const size_t handshake_timeout = 250;
            static const unsigned char echo_enabled_verbosity_disabled[] = "E1V0";
            static const unsigned char write_active_profile[] = "&W";
            switch (atrpc->handshake_machine_state)
            {
              case 0:
                // Negotiate auto-bauding by sending "AT\r" until the message is acknowledged with an "0\r", "\r\nOK\r\n", "AT\r0\r" or "AT\r\r\nOK\r\n"
                /* Codes_SRS_ATRPC_27_056: [ If the ping times-out when negotiating auto-baud, then modem_on_bytes_received() shall reissue the ping by calling (int)xio_send(XIO_HANDLE handle, const void * buffer, size_t size, ON_IO_SEND_COMPLETE on_io_send_complete, void * on_io_send_context) using the xio handle returned from xio_create() for the handle parameter, and "AT\r" for the buffer parameter, and "3" for the size parameter. ] */
                if (0 != atrpc_attention(context_, NULL, 0, handshake_timeout, NULL, 0, modem_handshake, context_, NULL, NULL))
                {
                    LogError("XIO Unable to negotiate auto-bauding!");
                    /* Codes_SRS_ATRPC_27_057: [ If atrpc_attention() returns a non-zero value, then on_io_open_complete() shall call the on_open_complete callback passed to atrpc_open() using the on_open_complete_context parameter passed to atrpc_open() as the context parameter, "ERROR_3GPP" as the result_code parameter, and "NULL" as the response parameter. ] */
                    atrpc->on_open_complete(atrpc->on_open_complete_context, ERROR_ATRPC);
                }
                break;
              case 1:
                // Normalize terminal adapter responses by sending "ATE1V0\r"
                /* Codes_SRS_ATRPC_27_061: [ When auto-baud negotiation has completed, then modem_on_bytes_received() shall normalize the ta responses by calling (int)xio_send(XIO_HANDLE handle, const void * buffer, size_t size, ON_IO_SEND_COMPLETE on_io_send_complete, void * on_io_send_context) using the xio handle returned from xio_create() during atrpc_create() for the handle parameter, and "ATE1V0\r" for the buffer parameter, and "7" for the size parameter. ] */
                if (0 != atrpc_attention(context_, echo_enabled_verbosity_disabled, (sizeof(echo_enabled_verbosity_disabled) - 1), handshake_timeout, NULL, 0, modem_handshake, context_, NULL, NULL))
                {
                    LogError("XIO Unable to normalize the terminal adapter response syntax!");
                    /* Codes_SRS_ATRPC_27_062: [ If the call to attention() returns a non-zero value, then modem_handshake() shall call the (void)on_open_complete(void * context, ta_result_code result_code, char * response) callback provided to atrpc_open(), using the on_open_complete_context argument provided to atrpc_open() as the context parameter, "3GPP_ERROR" as the result_code parameter, and "NULL" as the response parameter. ] */
                    atrpc->on_open_complete(atrpc->on_open_complete_context, ERROR_ATRPC);
                }
                break;
              case 2:
                // Write the settings to the active profile by sending "AT&W\r"
                /* Codes_SRS_ATRPC_27_063: [ Once the communication with the modem has been normalized, modem_on_bytes_received() shall write the active profile by calling (int)attention(ATRPC_HANDLE const handle, const char * const command_string, const size_t command_string_length, const size_t timeout_ms, TA_RESPONSE const ta_response, const void * const ta_response_context), using the context argument for the handle parameter, "&W" as the command_string parameter, "2" as the command_string_length parameter, "0" as the timeout_ms parameter, modem_handshake as the ta_response parameter, and the context argument as the ta_response_context parameter. ] */
                if (0 != atrpc_attention(context_, write_active_profile, (sizeof(write_active_profile) - 1), handshake_timeout, NULL, 0, modem_handshake, context_, NULL, NULL))
                {
                    LogError("XIO Unable to write the active profile!");
                    /* Codes_SRS_ATRPC_27_064: [ If the call to attention() returns a non-zero value, then modem_handshake() shall call the (void)on_open_complete(void * context, ta_result_code result_code, char * response) callback provided to atrpc_open(), using the on_open_complete_context argument provided to atrpc_open() as the context parameter, 3GPP_ERROR as the result_code parameter, and "XIO Unable to write the active profile!" as the response parameter. ] */
                    atrpc->on_open_complete(atrpc->on_open_complete_context, ERROR_ATRPC);
                }
                break;
              case 3:
                LogInfo("Handshake successful!");
                atrpc->status = ATRPC_OPEN;
                /* Codes_SRS_ATRPC_27_066: [ Once the profile has been successfully stored, then modem_on_bytes_received() shall call the (void)on_open_complete(void * context, ta_result_code result_code, char * response) callback provided to atrpc_open(), using the on_open_complete_context argument provided to atrpc_open() as the context parameter, 3GPP_OK as the result_code parameter, and NULL as the response parameter. ] */
                atrpc->on_open_complete(atrpc->on_open_complete_context, OK_3GPP);
                break;
            }
        }
    }
}


static void modem_on_bytes_received (void * context_, const unsigned char * buffer_, size_t size_)
{
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_on_bytes_received()!");
    } else {
        if (ATRPC_CLOSED == atrpc->status)
        {
            /* Codes_SRS_ATRPC_27_052: [ If atrpc_open() has not been called on the handle (passed in the callback context), then modem_on_bytes_received() shall discard all bytes. ] */
        }
        else
        {
            bool response_complete = false;
            size_t index = 0;
            for (; index < size_ && !response_complete; ++index)
            {
                unsigned char received_byte = buffer_[index];

                if (ATRPC_NEGOTIATING_AUTOBAUD == atrpc->status)
                {
                    response_complete = autobaud_negotiation_machine(context_, buffer_[index]);
                    if ( response_complete )
                    {
                        atrpc->status = ATRPC_HANDSHAKING;
                        atrpc->ta_result_code = OK_3GPP;
                    }
                }
                else
                {
                    /* Codes_SRS_ATRPC_27_053: [ If the `on_open_complete()` callback has been called, then `modem_on_bytes_received()` shall store any bytes following the prefix of the `command_string` parameter passed to `attention()` along with the postfixed `<result code>"\r"` in the buffer supplied to `attention()`. ] */
                    if (atrpc->awaiting_echo)
                    {
                        if (atrpc->current_request[atrpc->echo_machine_state] == received_byte)
                        {
                            ++atrpc->echo_machine_state;
                            if (atrpc->current_request_length == atrpc->echo_machine_state)
                            {
                                atrpc->awaiting_echo = false;
                                atrpc->echo_machine_state = 0;
                                atrpc->ta_response_buffer_index = 0;
                                atrpc->ta_result_code_machine_state = 1;
                            }
                        }
                        else
                        {
                            atrpc->echo_machine_state = 0;
                        }
                    }
                    else
                    {
                        if (atrpc->ta_response_buffer_index < atrpc->ta_response_buffer_allocated_size)
                        {
                            atrpc->response_buffer[atrpc->ta_response_buffer_index++] = received_byte;
                        }
                        if (NULL != atrpc->ta_result_code_parser) {
                            /* Codes_SRS_ATRPC_27_081: [ When a CUSTOM_RESULT_CODE_PARSER callback is supplied to attention(), modem_on_bytes_received() shall call the callback with each byte to determine the end of a response instead of searching for a standard result code. ] */
                            response_complete = atrpc->ta_result_code_parser(atrpc->ta_result_code_parser_context, received_byte, &atrpc->ta_result_code);
                        } else {
                            switch (atrpc->ta_result_code_machine_state)
                            {
                              case 0:
                                if ('\r' == received_byte)
                                {
                                    atrpc->ta_result_code_machine_state = 1;
                                }
                                break;
                              case 1:
                                // Accept all valid result codes
                                if (0x35 != received_byte && 0x30 <= received_byte && 0x39 >= received_byte)
                                {
                                    atrpc->ta_result_code = (TA_RESULT_CODE)(received_byte - 0x30);
                                    atrpc->ta_result_code_machine_state = 2;
                                }
                                else if ('\r' == received_byte)
                                {
                                    /* defer */
                                }
                                else if ('\n' == received_byte)
                                {
                                    atrpc->ta_result_code_machine_state = 3;
                                }
                                else
                                {
                                    atrpc->ta_result_code_machine_state = 0;
                                }
                                break;
                              case 2:
                                if ('\r' == received_byte)
                                {
                                    response_complete = true;
                                }
                                atrpc->ta_result_code_machine_state = 0;
                                break;
                              case 3:
                                // Accept all valid result codes
                                if (0x35 != received_byte && 0x30 <= received_byte && 0x39 >= received_byte)
                                {
                                    atrpc->ta_result_code = (TA_RESULT_CODE)(received_byte - 0x30);
                                    atrpc->ta_result_code_machine_state = 2;
                                }
                                else if ('\r' == received_byte)
                                {
                                    atrpc->ta_result_code_machine_state = 1;
                                }
                                else
                                {
                                    atrpc->ta_result_code_machine_state = 0;
                                }
                                break;
                              default:
                                break;
                            }
                        }
                    }
                }
                if (response_complete)
                {
                    /* Codes_SRS_ATRPC_27_060: [ Once a complete response has been received, then modem_on_bytes_received() shall free the stored command string. ] */
                    clear_current_request(atrpc);
                                                 
                    /* Codes_SRS_ATRPC_27_054: [ If any bytes where captured, `modem_on_bytes_received()` shall call the `ta_response` callback passed to `attention()` using the `ta_response_context` as the `context` parameter, the captured result code as the `result_code` parameter, a pointer to the buffer as the `message` parameter, and the size of the received message as size. ] */
                    atrpc->on_ta_response(atrpc->on_ta_response_context, atrpc->ta_result_code, atrpc->response_buffer, atrpc->ta_response_buffer_index);
                }
            }
        }
    }
    return;
}


static void modem_on_io_close_complete (void * context_)
{
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_on_io_close_complete()!");
    }
    else
    {
        atrpc->modem_status = MODEM_IO_CLOSED;
        /* Codes_SRS_ATRPC_27_067: [ on_io_close_complete() shall call nothing. ] */
    }
    return;
}


static void modem_on_io_error (void * context_)
{
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_on_io_error()!");
    }
    else
    {
        LogError("XIO buffer error!");
        /* Codes_SRS_ATRPC_27_068: [ on_io_error() shall free the stored command string. ] */
        clear_current_request(atrpc);

        /* Codes_SRS_ATRPC_27_069: [ on_io_error() shall call the terminal adapter response callback passed as ta_response to attention() using the ta_response_context parameter passed to attention() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the message parameter. ] */
        atrpc->on_ta_response(atrpc->on_ta_response_context, ERROR_ATRPC, NULL, 0);
    }
    return;
}


static void modem_on_io_open_complete (void * context_, IO_OPEN_RESULT open_result_)
{
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_on_io_open_complete()!");
    }
    else
    {
        if (IO_OPEN_OK != open_result_)
        {
            LogError("Unable to open underlying xio!");
            atrpc->handshake_attempt = 0;
            atrpc->handshake_machine_state = 0;
            atrpc->modem_status = MODEM_IO_CLOSED;
            atrpc->status = ATRPC_CLOSED;
            /* Codes_SRS_ATRPC_27_070: [ If the open_result parameter is not IO_OPEN_OK, then on_io_open_complete() shall call the on_open_complete callback passed to atrpc_open() using the on_open_complete_context parameter passed to atrpc_open() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the response parameter. ] */
            atrpc->on_open_complete(atrpc->on_open_complete_context, ERROR_ATRPC);
        }
        else
        {
            atrpc->modem_status = MODEM_IO_OPEN;
            /* Codes_SRS_ATRPC_27_071: [ If the open_result parameter is OPEN_OK, then on_io_open_complete() shall initiate the auto-bauding procedure by calling (void)atrpc_attention(void * context, const char * const command_string, const size_t command_string_length const size_t timeout_ms, TA_RESPONSE const ta_response, const void * const ta_response_context) using the incoming context parameter as the handle parameter, NULL as the command_string parameter, 0 as the command_string_length parameter, 100 as the timeout_ms parameter, modem_handshake as the ta_response parameter, and ta_response_context as the context parameter. ] */
            /* Codes_SRS_ATRPC_27_072: [ If atrpc_attention returns a non-zero value, then on_io_open_complete() shall call the on_open_complete callback passed to atrpc_open() using the on_open_complete_context parameter passed to atrpc_open() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the response parameter. ] */
            modem_handshake(context_, ERROR_ATRPC, NULL, 0);
        }
    }
    return;
}


static void modem_on_send_complete (void * context_, IO_SEND_RESULT send_result_)
{
    ATRPC_INSTANCE * atrpc = (ATRPC_INSTANCE *)context_;

    if (NULL == context_)
    {
        LogError("NULL context passed into modem_on_io_open_complete()!");
    }
    else
    {
        atrpc->modem_receiving = false;
        if (IO_SEND_OK != send_result_)
        {
            LogError("Unable to send via underlying xio!");
            /* Codes_SRS_ATRPC_27_075: [ If the result of underlying xio on_io_send_complete() is not IO_SEND_OK, then on_send_complete() shall free the command string passed to attention(). ] */
            clear_current_request(atrpc);

            /* Codes_SRS_ATRPC_27_074: [ If the result of underlying xio on_io_send_complete() is IO_SEND_CANCELLED, then on_send_complete() shall call the terminal adapter response callback passed as ta_response to attention() using the ta_response_context parameter passed to attention() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the message parameter. ] */
            /* Codes_SRS_ATRPC_27_076: [ If the result of underlying xio on_io_send_complete() is IO_SEND_ERROR, then on_send_complete() shall call the terminal adapter response callback passed as ta_response to attention() using the ta_response_context parameter passed to attention() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the message parameter. ] */
            atrpc->on_ta_response(atrpc->on_ta_response_context, ERROR_ATRPC, NULL, 0);
        }
        else
        {
            /* Codes_SRS_ATRPC_27_073: [ If the result of underlying xio on_io_send_complete() is IO_SEND_OK, then on_send_complete() shall call nothing. ] */
        }
    }
    return;
}


int atrpc_attention (ATRPC_HANDLE handle_, const unsigned char * command_string_, size_t command_string_length_, size_t timeout_ms_, unsigned char * ta_response_buffer_, size_t ta_response_buffer_size_, ON_TA_RESPONSE on_ta_response_, void * on_ta_response_context_, CUSTOM_TA_RESULT_CODE_PARSER ta_result_code_parser_, void * ta_result_code_parser_context_)
{
    int result;

    if (NULL == handle_)
    {
        /* Codes_SRS_ATRPC_27_000: [ If the handle argument is NULL, then atrpc_attention() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("NULL handle passed to atrpc_attention()!");
    }
    else if (NULL == on_ta_response_)
    {
        /* Codes_SRS_ATRPC_27_001: [ If the on_ta_response argument is NULL, then atrpc_attention() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("NULL callback passed to atrpc_attention()!");
    }
    else if (0 != command_string_length_ && NULL == command_string_)
    {
        /* Codes_SRS_ATRPC_27_085: [ If `command_string_length` is not zero and `command_string` is `NULL`, then `atrpc_attention()` shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("Previous call to atrpc_attention() has not returned!");
    }
    else if (0 != ta_response_buffer_size_ && NULL == ta_response_buffer_)
    {
        /* Codes_SRS_ATRPC_27_086: [ If `ta_response_buffer_size` is not zero and `response_buffer` is not `NULL`, then `atrpc_attention()` shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("Previous call to atrpc_attention() has not returned!");
    }
    else if (MODEM_IO_OPEN != handle_->modem_status)
    {
        /* Codes_SRS_ATRPC_27_002: [ If the on_io_open_complete() callback from the underlying xio has not been called, then atrpc_attention() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("atrpc_attention() unable to send - underlying XIO is not open!");
    }
    else if (NULL != handle_->current_request)
    {
        /* Codes_SRS_ATRPC_27_003: [ If a command is currently outstanding, then atrpc_attention() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("Previous call to atrpc_attention() has not returned!");
    }
    else
    {
        handle_->current_request_length = (command_string_length_ + (sizeof("AT\r") - 1));

        /* Codes_SRS_ATRPC_27_004: [ atrpc_attention() shall mark the call time, by calling (int)tickcounter_get_current_ms(TICKCOUNTER_HANDLE handle, tickcounter_ms_t * current_ms) using the handle returned from atrpc_create() as the handle parameter. ] */
        if (0 != tickcounter_get_current_ms(handle_->tick_counter, &handle_->call_origination_ms))
        {
            /* Codes_SRS_ATRPC_27_005: [ If the call to tickcounter_get_current_ms() returns a non-zero value, then atrpc_attention() shall fail and return a non-zero value. ] */
            result = __LINE__;
            LogError("atrpc_attention() unable to timestamp the transaction!");
        /* Codes_SRS_ATRPC_27_006: [ atrpc_attention() store the command string, by calling (void *)malloc(size_t size) using (command_string_length + 3) for the size parameter. ] */
        }
        else if (NULL == (handle_->current_request = malloc(handle_->current_request_length)))
        {
            /* Codes_SRS_ATRPC_27_007: [ If the call to malloc() returns NULL, then atrpc_attention() shall fail and return a non-zero value. ] */
            result = __LINE__;
            LogError("atrpc_attention() unable to store request string!");
        }
        else
        {
            /* Codes_SRS_ATRPC_27_008: [ atrpc_attention() shall call (int)xio_send(XIO_HANDLE handle, const void * buffer, size_t size, ON_IO_SEND_COMPLETE on_io_send_complete, void * on_io_send_context) using the xio handle returned from xio_create() for the handle parameter, and AT<command_string>\r for the buffer parameter, and (command_string_length + 3) for the size parameter. ] */
            handle_->current_request[0] = 'A';
            handle_->current_request[1] = 'T';
            (void)memcpy(&handle_->current_request[2], command_string_, command_string_length_);
            handle_->current_request[(handle_->current_request_length - 1)] = '\r';
            handle_->modem_receiving = true;
            handle_->on_ta_response = on_ta_response_;
            handle_->on_ta_response_context = on_ta_response_context_;
            handle_->response_buffer = ta_response_buffer_;
            handle_->ta_response_buffer_allocated_size = ta_response_buffer_size_;
            handle_->ta_result_code_parser = ta_result_code_parser_;
            handle_->ta_result_code_parser_context = ta_result_code_parser_context_;
            handle_->timeout_ms = timeout_ms_;

            if (0 != xio_send(handle_->modem_io, handle_->current_request, handle_->current_request_length, modem_on_send_complete, handle_))
            {
                /* Codes_SRS_ATRPC_27_009: [ If the call to xio_send() returns a non-zero value, then atrpc_attention() shall fail and return a non-zero value. ] */
                result = __LINE__;
                /* Codes_SRS_ATRPC_27_012: [ VALGRIND - If the call to xio_send() returns a non-zero value, then the stored command string shall be freed. ] */
                clear_current_request(handle_);
                handle_->modem_receiving = false;
                LogError("atrpc_attention() failed to send request to underlying xio!");
            }
            else
            {
                /* Codes_SRS_ATRPC_27_010: [ atrpc_attention() shall block until the on_send_complete callback passed to xio_send() returns. ] */
                assert(!handle_->modem_receiving);

                /* Codes_SRS_ATRPC_27_011: [ If no errors are encountered during execution, then atrpc_attention() shall return 0. ] */
                result = 0;
            }
        }
    }
    return result;
}


int atrpc_close (ATRPC_HANDLE handle_)
{
    int result;
    
    if (NULL == handle_)
    {
        /* Codes_SRS_ATRPC_27_013: [ If the handle argument is NULL, then atrpc_close() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("NULL handle passed to atrpc_close()!");
    }
    else if (ATRPC_CLOSED == handle_->status)
    {
        /* Codes_SRS_ATRPC_27_014: [ If atrpc_open() has not been called on the handle, atrpc_close() shall do nothing and return 0. ] */
        result = 0;
        LogInfo("AT RPC is already closed.");
    /* Codes_SRS_ATRPC_27_016: [ atrpc_close() shall call (int)xio_close(XIO_HANDLE handle, ON_IO_CLOSE_COMPLETE on_io_close_complete, void * on_io_close_complete_context). ] */
    }
    else if (0 != xio_close(handle_->modem_io, modem_on_io_close_complete, handle_))
    {
        /* Codes_SRS_ATRPC_27_017: [ If the call to xio_close() returns a non-zero value, then atrpc_close() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("atrpc_close() unable to close underlying xio!");
    }
    else
    {
        /* Codes_SRS_ATRPC_27_018: [ atrpc_close() shall block until the on_io_close_complete callback passed to xio_close() completes. ] */
        assert(MODEM_IO_CLOSED == handle_->modem_status);

        if (ATRPC_NEGOTIATING_AUTOBAUD == handle_->status || ATRPC_HANDSHAKING == handle_->status)
        {
        	clear_current_request(handle_);
            /* Codes_SRS_ATRPC_27_015: [ If atrpc_open() has been called on the handle and the on_open_complete callback has not been called, atrpc_close() shall call the (void)on_open_complete(void * context, ta_result_code result_code, char * response) callback provided to atrpc_open(), using the on_open_complete_context argument provided to atrpc_open() as the context parameter, and ERROR_ATRPC as the result_code parameter. ] */
            handle_->on_open_complete(handle_->on_open_complete_context, ERROR_ATRPC);
        }

        handle_->status = ATRPC_CLOSED;
        /* Codes_SRS_ATRPC_27_019: [ If no errors are encountered during execution, then atrpc_close() shall return 0. ] */
        result = 0;
    }

    return result;
}


ATRPC_HANDLE atrpc_create (void)
{
    ATRPC_HANDLE result;
    const IO_INTERFACE_DESCRIPTION * xio_ifc;
    const UARTIO_CONFIG xio_config = {9600, 8};

    /* Codes_SRS_ATRPC_27_020: [ atrpc_create() shall call malloc() to allocate the memory required for the internal data structure. ] */
    if (NULL == (result = (ATRPC_INSTANCE *)calloc(sizeof(ATRPC_INSTANCE), sizeof(unsigned char))))
    {
        /* Codes_SRS_ATRPC_27_021: [ If malloc() fails to allocate the memory required for the internal data structure, then atrpc_create() shall fail and return a NULL handle. ] */
        LogError("atrpc_create() unable to allocate memory for internal data structure!");
    }
    /* Codes_SRS_ATRPC_27_022: [ atrpc_create() shall create a tickcounter to support timeout functionality by calling (TICKCOUNTER_HANDLE)tickcounter_create(void). ] */
    else if (NULL == (result->tick_counter = tickcounter_create()))
    {
        /* Codes_SRS_ATRPC_27_023: [ If the call to tickcounter_create() returns NULL, then atrpc_create() shall fail and return NULL. ] */
        free(result);
        result = NULL;
        LogError("atrpc_create() unable to create tick counter!");
    }
    /* Codes_SRS_ATRPC_27_024: [ atrpc_create() shall acquire an xio interface to a modem chipset by calling (IO_INTERFACE_DESCRIPTION *)uartio_get_interface_description(). ] */
    else if (NULL == (xio_ifc = (IO_INTERFACE_DESCRIPTION *)uartio_get_interface_description()))
    {
        /* Codes_SRS_ATRPC_27_025: [ If the call to uartio_get_interface_description() returns NULL, then atrpc_create() shall fail and return NULL. ] */
        tickcounter_destroy(result->tick_counter);
        free(result);
        result = NULL;
        LogError("atrpc_create() unable to obtain interface description for underlying xio layer!");
    }
    /* Codes_SRS_ATRPC_27_026: [ atrpc_create() shall create an xio connection to a modem chipset by calling (XIO_HANDLE)xio_create(const IO_INTERFACE_DESCRIPTION * io_interface_description, const void * io_create_parameters) using the interface description returned from uartio_get_interface_description() for io_interface_description. ] */
    else if (NULL == (result->modem_io = xio_create(xio_ifc, &xio_config)))
    {
        /* Codes_SRS_ATRPC_27_027: [ If the call to xio_create() returns NULL, then atrpc_create() shall fail and return NULL. ] */
        tickcounter_destroy(result->tick_counter);
        free(result);
        result = NULL;
        LogError("atrpc_create() unable to create underlying xio layer!");
    }
    else
    {
        // Additional configuration
    }

    /* Codes_SRS_ATRPC_27_030: [ VALGRIND - When atrpc_create() returns a non-zero value, all allocated resources up to that point shall be freed. ] */
    /* Codes_SRS_ATRPC_27_031: [ If no errors are encountered during execution, atrpc_create() shall return a handle to an AT RPC instance. ] */
    return (ATRPC_HANDLE)result;
}


void atrpc_destroy (ATRPC_HANDLE handle_)
{
    if (NULL == handle_)
    {
        /* Codes_SRS_ATRPC_27_032: [ If the handle argument is NULL, then atrpc_destroy() shall do nothing. ] */
        LogError("NULL handle passed to atrpc_destroy()!");
    }
    else
    {
        /* Codes_SRS_ATRPC_27_033: [ If atrpc_open() has previously been called and atrpc_close() has not been called on the handle, atrpc_destroy() shall call (int)atrpc_close(ATRPC_HANDLE handle) using the handle argument passed to atrpc_destroy() as the handle parameter. ] */
        if (ATRPC_CLOSED != handle_->status)
        {
            LogInfo("ATRPC handle was not closed before atrpc_destroy() was called.");
            if (0 != atrpc_close(handle_))
            {
                LogError("atrpc_destroy() unable to properly close atrpc!");
            }
        }

        /* Codes_SRS_ATRPC_27_034: [ atrpc_destroy() shall call (void)xio_destroy(XIO_HANDLE handle) using the handle returned from the call to xio_create() for the handle parameter. ] */
        xio_destroy(handle_->modem_io);

        /* Codes_SRS_ATRPC_27_036: [ atrpc_destroy() shall call (void)tickcounter_destroy(TICKCOUNTER_HANDLE handle) using the handle returned from the call to tickcounter_create() for the handle parameter. ] */
        tickcounter_destroy(handle_->tick_counter);

        /* Codes_SRS_ATRPC_27_037: [ atrpc_destroy() shall free the memory required for current request. ] */
        free(handle_->current_request);

        /* Codes_SRS_ATRPC_27_038: [ atrpc_destroy() shall free the memory required for the internal data structure. ] */
        free(handle_);
    }
}


void atrpc_dowork (ATRPC_HANDLE handle_)
{
    if (NULL == handle_)
    {
        /* Codes_SRS_ATRPC_27_039: [ If the handle argument is NULL, then atrpc_dowork() shall do nothing. ] */
        LogError("NULL handle passed to atrpc_dowork()");
    }
    else
    {
        tickcounter_ms_t ms;

        /* Codes_SRS_ATRPC_27_040: [ atrpc_dowork() shall call (void)xio_dowork(XIO_HANDLE handle) using the handle returned from the call to xio_create() for the handle parameter. ] */
        xio_dowork(handle_->modem_io);

        // Timeout
        if (ATRPC_CLOSED == handle_->status)
        {
            // Nothing to timeout when closed
        }
        else if (0 == handle_->timeout_ms)
        {
            /* Codes_SRS_ATRPC_27_043: [ If attention() was called with a timeout of 0, then dowork() shall not attempt to calculate a timeout. ] */
        }
        /* Codes_SRS_ATRPC_27_041: [ If atrpc_open() has been called on the handle, atrpc_dowork() shall mark the call time, by calling (int)tickcounter_get_current_ms(TICKCOUNTER_HANDLE handle, tickcounter_ms_t * current_ms) using the handle returned from atrpc_create() as the handle parameter. ] */
        else if (0 != tickcounter_get_current_ms(handle_->tick_counter, &ms))
        {
            /* Codes_SRS_ATRPC_27_042: [ If tickcounter_get_current_ms() returns a non-zero value, then dowork() shall not attempt to calculate a timeout. ] */
            LogError("atrpc_dowork() received an error from tickcounter_get_current_ms()");
        }
        else if ((ms - handle_->call_origination_ms) >= handle_->timeout_ms)
        {
            /* Codes_SRS_ATRPC_27_044: [ If atrpc_open() has been called on the handle, and the timeout value sent as timeout_ms to the originating attention() call is non-zero and has expired, then atrpc_dowork() shall free the stored command string. ] */
            clear_current_request(handle_);

            LogInfo("atrpc_dowork() timed-out current AT RPC request.");
            /* Codes_SRS_ATRPC_27_045: [ If atrpc_open() has been called on the handle, and the timeout value sent as timeout_ms to the originating attention() call is non-zero and has expired, then atrpc_dowork() shall call the terminal adapter response callback passed as on_ta_response to attention() using the ta_response_context parameter passed to attention() as the context parameter, ERROR_ATRPC as the result_code parameter, and NULL as the message parameter. ] */
            handle_->on_ta_response(handle_->on_ta_response_context, ERROR_ATRPC, NULL, 0);
        }
    }
}


int atrpc_open (ATRPC_HANDLE handle_, ON_OPEN_COMPLETE on_open_complete_, void * on_open_complete_context_)
{
    int result;
    if (NULL == handle_)
    {
        /* Codes_SRS_ATRPC_27_046: [ If the handle argument is NULL, then atrpc_open() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("NULL handle passed to atrpc_open()!");
    }
    else if (NULL == on_open_complete_)
    {
        /* Codes_SRS_ATRPC_27_047: [ If the on_open_complete argument is NULL, then atrpc_open() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("NULL callback passed to atrpc_open()!");
    }
    else if (ATRPC_CLOSED != handle_->status)
    {
        /* Codes_SRS_ATRPC_27_048: [ If atrpc_open() has been called previously and atrpc_close() has not been called on the handle, atrpc_open() shall fail and return a non-zero value. ] */
        result = __LINE__;
        LogError("atrpc_open() requested on active handle!");
    }
    /* Codes_SRS_ATRPC_27_049: [ atrpc_open() shall call (int)xio_open(XIO_HANDLE handle, ON_IO_OPEN_COMPLETE on_io_open_complete, void * on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void * on_bytes_received_context, ON_IO_ERROR on_io_error, void * on_io_error_context) using the handle returned from xio_create() as the handle parameter, the incoming handle parameter as the on_bytes_received_context parameter, and the incoming handle parameter as the on_io_open_complete_context parameter. ] */
    else
    {
        // Must happen before call to open(), as these variables are affected by open() which can return immediately
        handle_->echo_machine_state = 0;
        handle_->handshake_attempt = 0;
        handle_->handshake_machine_state = 0;
        handle_->modem_status = MODEM_IO_OPENING;
        handle_->on_open_complete = on_open_complete_;
        handle_->on_open_complete_context = on_open_complete_context_;
        handle_->ta_result_code_machine_state = 0;
        handle_->status = ATRPC_NEGOTIATING_AUTOBAUD;

        if (0 != xio_open(handle_->modem_io, modem_on_io_open_complete, handle_, modem_on_bytes_received, handle_, modem_on_io_error, handle_))
        {
            /* Codes_SRS_ATRPC_27_050: [ If xio_open() returns a non-zero value, then atrpc_open() shall do nothing and return a non-zero value. ] */
            result = __LINE__;
            LogError("atrpc_open() failed to open underlying xio layer!");
        }
        else
        {
            /* Codes_SRS_ATRPC_27_051: [ If no errors are encountered, atrpc_open() shall return 0. ] */
            result = 0;
        }
    }

    /* Codes_SRS_ATRPC_27_077: [ If any errors are encountered, atrpc_open() shall call on_open_complete using the on_open_complete_context, and a TA_RESULT_CODE of ERROR_ATRPC. ] */  
    if (0 != result && NULL != on_open_complete_)
    {
        on_open_complete_(on_open_complete_context_, ERROR_ATRPC);
    }

    return result;
}

