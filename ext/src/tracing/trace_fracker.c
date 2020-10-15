/* Copyright (c) 2020 Andrea Cardaci <cyrus.and@gmail.com> */

#include "php.h"
#include "ext/standard/php_string.h"

#include "php_xdebug.h"
#include "tracing_private.h"
#include "trace_fracker.h"

#include "lib/var_export_line.h"

#include "ext/json/php_json.h"
#include "zend_smart_str.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <json.h>

#define LOG_PREFIX "[!] Fracker: "

#define CTXT(x) (((xdebug_trace_fracker_context*) ctxt)->x)

extern ZEND_DECLARE_MODULE_GLOBALS(xdebug);

static int connect_to_server()
{
    struct addrinfo *addresses, *ptr, hints = {0};
    int errorcode, socket_fd = -1;

    /* resolve the given address */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    errorcode = getaddrinfo(XINI_TRACE(trace_fracker_host), XINI_TRACE(trace_fracker_port), &hints, &addresses);
    if (errorcode) {
        return -1;
    }

    /* try all the available addresses */
    for (ptr = addresses; ptr; ptr = ptr->ai_next) {
        /* allocate a socket file descriptor */
        socket_fd = socket(ptr->ai_family, ptr->ai_socktype, 0);
        if (socket_fd == -1) {
            return -1;
        }

        /* connect to the server */
        if (connect(socket_fd, ptr->ai_addr, ptr->ai_addrlen)) {
            close(socket_fd);
            continue;
        }

        /* connection successful */
        break;
    }

    /* cleanup address resolution data */
    freeaddrinfo(addresses);

    /* connection not performed */
    if (!ptr) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void write_json_object(int fd, struct json_object *object)
{
    struct iovec to_write[2];

    /* TODO properly check the writev syscall */

    /* write the object followed by a newline then cleanup */
    to_write[0].iov_base = (void *)json_object_to_json_string(object);
    to_write[0].iov_len = strlen(to_write[0].iov_base);
    to_write[1].iov_base = "\n";
    to_write[1].iov_len = 1;
    writev(fd, to_write, 2);
    json_object_put(object);
}

static int zval_to_json(zval *value, struct json_object **object)
{
    smart_str buf = {0};

    /* XXX zval -> JSON using PHP api, then parse it back */

    /* XXX JSON_G(error_code) may cause undefined symbol at runtime so we rely
       on the fact that the string is not NULL */

    php_json_encode(&buf, value, PHP_JSON_PARTIAL_OUTPUT_ON_ERROR);
    if (buf.s) {
        smart_str_0(&buf);
        *object = json_tokener_parse(ZSTR_VAL(buf.s));
        smart_str_free(&buf);
        return 1;
    } else {
        /* fall back to null */
        *object = NULL;
        return 0;
    }
}

static void add_json_zval(void *ctxt, struct json_object *parent, const char *key, zval *value)
{
    struct json_object *object = NULL;

    /* convert the zvalue and notify the server on errors (a null object is used) */
    if (!zval_to_json(value, &object)) {
        struct json_object *info;
        xdebug_str *tmp_value, message = XDEBUG_STR_INITIALIZER;

        /* prepare message */
        xdebug_str_add(&message, "Invalid JSON conversion for ", 0);
        tmp_value = xdebug_get_zval_value_line(value, 0, NULL);
        xdebug_str_add_str(&message, tmp_value);

        /* send warning info */
        info = json_object_new_object();
        json_object_object_add(info, "type", json_object_new_string("warning"));
        json_object_object_add(info, "message", json_object_new_string(message.d));
        write_json_object(CTXT(socket_fd), info);
        fprintf(stderr, LOG_PREFIX "%s\n", message.d);

        /* cleanup */
        xdebug_str_free(tmp_value);
        xdebug_str_destroy(&message);
    }

    /* update the json object */
    json_object_object_add(parent, key, object);
}

static void add_json_typed_zval(void *ctxt, struct json_object *parent, zval *value)
{
    xdebug_str *type;

    /* add type */
    add_json_zval(ctxt, parent, "value", value);

    /* add value */
    type = xdebug_get_zval_synopsis_line(value, 0, NULL);
    json_object_object_add(parent, "type", json_object_new_string(type->d));
    xdebug_str_free(type);
}

static struct json_object *get_php_input()
{
    php_stream *php_input;
    json_object *input = NULL;
    zend_string *data = NULL;

    /* open the PHP inpuit stream (POST data or stdin) and read its value */
    php_input = php_stream_open_wrapper("php://input", "r", 0, NULL);
    data = php_stream_copy_to_mem(php_input, PHP_STREAM_COPY_ALL, 0);
    if (data) {
        input = json_object_new_string_len(ZSTR_VAL(data), ZSTR_LEN(data));
        zend_string_free(data);
    }

    php_stream_close(php_input);
    return input;
}

void *xdebug_trace_fracker_init(char *fname, char *script_filename, long options TSRMLS_DC)
{
    xdebug_trace_fracker_context *ctxt;
    int socket_fd;

    /* establish a connection to the server */
    socket_fd = connect_to_server();
    if (socket_fd == -1) {
        fprintf(stderr, LOG_PREFIX "Cannot connect to %s:%s\n",
                XINI_TRACE(trace_fracker_host), XINI_TRACE(trace_fracker_port));
        return NULL;
    }

    /* allocate and populate the context */
    ctxt = xdmalloc(sizeof(xdebug_trace_fracker_context));
    CTXT(socket_fd) = socket_fd;
    return ctxt;
}

void xdebug_trace_fracker_deinit(void *ctxt TSRMLS_DC)
{
    /* release resources */
    close(CTXT(socket_fd));
    xdfree(ctxt);
}

void xdebug_trace_fracker_write_header(void *ctxt TSRMLS_DC)
{
    struct json_object *info;

    /* send request info */
    info = json_object_new_object();
    json_object_object_add(info, "type", json_object_new_string("request"));
    add_json_zval(ctxt, info, "server", &PG(http_globals)[TRACK_VARS_SERVER]);
    add_json_zval(ctxt, info, "get", &PG(http_globals)[TRACK_VARS_GET]);
    add_json_zval(ctxt, info, "post", &PG(http_globals)[TRACK_VARS_POST]);
    add_json_zval(ctxt, info, "cookie", &PG(http_globals)[TRACK_VARS_COOKIE]);
    json_object_object_add(info, "input", get_php_input());
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_write_footer(void *ctxt TSRMLS_DC) {}

char *xdebug_trace_fracker_get_filename(void *ctxt TSRMLS_DC)
{
    return (char *)"{TCP}";
}

void xdebug_trace_fracker_function_entry(void *ctxt, function_stack_entry *fse, int function_nr TSRMLS_DC)
{
    struct json_object *info, *arguments, *argument;
    char *function;

    /* fill call info */
    function = xdebug_show_fname(fse->function, 0, 0 TSRMLS_CC);
    info = json_object_new_object();
    json_object_object_add(info, "type", json_object_new_string("call"));
    json_object_object_add(info, "id", json_object_new_int(fse->function_nr));
    json_object_object_add(info, "level", json_object_new_int(fse->level));
    json_object_object_add(info, "timestamp", json_object_new_double(xdebug_get_utime()));
    json_object_object_add(info, "function", json_object_new_string(function));
    json_object_object_add(info, "file", json_object_new_string(fse->filename));
    json_object_object_add(info, "line", json_object_new_int(fse->lineno));
    xdfree(function);

    /* process arguments */
    arguments = json_object_new_array();
    if (fse->include_filename) {
        /* XXX require and include are handled differently (unfortunately this
           is not the actual variable value but a computed one) */

        /* fill and add argument info */
        argument = json_object_new_object();
        json_object_object_add(argument, "value", json_object_new_string(fse->include_filename));
        json_object_array_add(arguments, argument);
    } else {
        int i;

        for (i = 0; i < fse->varc; i++) {
            const char *name;

            /* fill and add argument info */
            name = fse->var[i].name;
            argument = json_object_new_object();
            if (name) {
                json_object_object_add(argument, "name", json_object_new_string(name));
            }
            add_json_typed_zval(ctxt, argument, &fse->var[i].data);
            json_object_array_add(arguments, argument);
        }
    }
    json_object_object_add(info, "arguments", arguments);

    /* serialize and send */
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_function_exit(void *ctxt, function_stack_entry *fse, int function_nr TSRMLS_DC)
{
    struct json_object *info;

    /* fill call info */
    info = json_object_new_object();
    json_object_object_add(info, "type", json_object_new_string("exit"));
    json_object_object_add(info, "id", json_object_new_int(fse->function_nr));
    json_object_object_add(info, "level", json_object_new_int(fse->level));
    json_object_object_add(info, "timestamp", json_object_new_double(xdebug_get_utime()));

    /* serialize and send */
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_function_return_value(void *ctxt, function_stack_entry *fse, int function_nr, zval *return_value TSRMLS_DC)
{
    struct json_object *info, *return_;

    /* fill call info */
    info = json_object_new_object();
    json_object_object_add(info, "type", json_object_new_string("return"));
    json_object_object_add(info, "id", json_object_new_int(fse->function_nr));
    json_object_object_add(info, "level", json_object_new_int(fse->level));

    /* process return value */
    return_ = json_object_new_object();
    add_json_typed_zval(ctxt, return_, return_value);
    json_object_object_add(info, "return", return_);

    /* serialize and send */
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_generator_return_value(void *ctxt, function_stack_entry *fse, int function_nr, zend_generator *generator TSRMLS_DC) {}

void xdebug_trace_fracker_assignment(void *ctxt, function_stack_entry *fse, char *full_varname, zval *value, char *right_full_varname, const char *op, char *file, int lineno TSRMLS_DC) {}

xdebug_trace_handler_t xdebug_trace_handler_fracker =
{
    xdebug_trace_fracker_init,
    xdebug_trace_fracker_deinit,
    xdebug_trace_fracker_write_header,
    xdebug_trace_fracker_write_footer,
    xdebug_trace_fracker_get_filename,
    xdebug_trace_fracker_function_entry,
    xdebug_trace_fracker_function_exit,
    xdebug_trace_fracker_function_return_value,
    xdebug_trace_fracker_generator_return_value,
    xdebug_trace_fracker_assignment
};
