#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <windows.h>

#include <curl/curl.h>
#include "cJSON.h"

#pragma comment(lib, "libcurl")

#define DEBUG

#ifdef DEBUG
    static FILE *fdbg;
    void dbg(const char *fmt, ...) {
        va_list args;
        if (!fdbg) fdbg = fopen("debug.txt", "a");
        va_start(args, fmt);
        vfprintf(fdbg, fmt, args);
        va_end(args);
        fprintf(fdbg, "-------------------------------------------------------------------------------\n");
    }
#else
    void dbg(const char *fmt, ...) {}
#endif

//////////////////////////////////////////////////////////////////////////
//
// CONFIGURATION

struct config_t {
    char openrouter_api_key[255];
    char openrouter_model[255];
};

const char *ini_file = "vc6ai.ini";

void config_init(struct config_t *config, const char *ini_file) {
    char cwd[MAX_PATH], ini_path[MAX_PATH];

    GetCurrentDirectoryA(MAX_PATH, cwd);
    _snprintf(ini_path, MAX_PATH, "%s\\%s", cwd, ini_file);

    GetPrivateProfileStringA("OpenRouter", "ApiKey", "",
        config->openrouter_api_key, sizeof(config->openrouter_api_key), ini_path);
    GetPrivateProfileStringA("OpenRouter", "Model", "deepseek/deepseek-v4-flash",
        config->openrouter_model, sizeof(config->openrouter_model), ini_path);
}

//////////////////////////////////////////////////////////////////////////
//
// TERMINAL

enum term_color {
    C_DARK_GRAY = FOREGROUND_INTENSITY,
    C_TURQUOISE = (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY),
    C_RED = (FOREGROUND_RED | FOREGROUND_INTENSITY),
    C_WHITE = (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
};

static HANDLE term_handle;
static WORD term_orig_attrs;

void term_init(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    term_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(term_handle, &csbi);
    term_orig_attrs = csbi.wAttributes;
}

void term_color(enum term_color color) {
    SetConsoleTextAttribute(term_handle, color);
};

void term_reset(void) {
    SetConsoleTextAttribute(term_handle, term_orig_attrs);
}

//////////////////////////////////////////////////////////////////////////
//
// HTTP

struct http_context_t {
    CURL *curl;
    struct curl_slist *headers;
    char *res;
    size_t reslen;
};

static size_t http_write_callback(char *data, size_t size, size_t nmemb, void *userdata) {
    struct http_context_t *ctx = (struct http_context_t *)userdata;
    size_t realsize = size * nmemb;

    char *ptr = realloc(ctx->res, ctx->reslen + realsize + 1);
    if (!ptr) return 0;

    ctx->res = ptr;
    memcpy(&(ctx->res[ctx->reslen]), data, realsize);
    ctx->reslen += realsize;
    ctx->res[ctx->reslen] = '\0'; // null terminate string as res buffer might not be empty

    return realsize;
}

int http_init(struct http_context_t *ctx, long timeout) {
    CURLcode res;

    res = curl_global_init(CURL_GLOBAL_ALL);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    ctx->curl = curl_easy_init();
    if (!ctx->curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return 2;
    }

    ctx->headers = NULL;
    ctx->res = NULL;
    ctx->reslen = 0;

    curl_easy_setopt(ctx->curl, CURLOPT_CAINFO, "curl-ca-bundle.crt");
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, ctx);

    return 0;
}

void http_cleanup(struct http_context_t *ctx) {
    if (ctx->curl) curl_easy_cleanup(ctx->curl);    
    curl_global_cleanup();
    if (ctx->headers) curl_slist_free_all(ctx->headers);
    if (ctx->res) free(ctx->res);

    memset(ctx, 0, sizeof(struct http_context_t));
}

//////////////////////////////////////////////////////////////////////////
//
// TOOLS

struct tool_params_t {
    const char *name;
    const char *type;
    const char *desc;
    int required;
};

struct tool_t {
    const char *name;
    const char *desc;
    const struct tool_params_t *params;
};

const struct tool_params_t run_cmd_params[] = {
    {
        "command",
        "string",
        /*
        // general 
        "Exact command to pass to Windows XP cmd.exe /C. "
        "The tool captures stdout and stderr automatically. "
        "Use one complete command; combine related steps with && to avoid extra tool calls. "
        "Use call \"file.bat\" when running a batch file before another command. "
        // file writing
        "For writing files, prefer one parenthesised block: "
        "(echo line1&echo line2&echo line3)>file.ext. "
        "Escape cmd metacharacters only inside echoed file content: ^< ^> ^| ^& ^( ^). "
        "Do not escape the final > or >> redirection operator used to write the file. "
        // internet access
        "curl.exe may be used for internet access. "
        // C code generation
        "When generating C to compile with cl/VC6, use C89-compatible C: "
        "declare variables at the start of a block, do not declare variables inside for loops, "
        "use int main(void), avoid // comments and avoid C99 features. "
        // command length 
        "Keep commands under about 4000 characters. "
        "For larger files, write them in several append blocks instead of one huge command.",*/

        // general
        "Exact command to pass to Windows XP cmd.exe /C. "
        "The tool captures stdout and stderr automatically. "
        "Use one complete command; combine related steps with && to avoid extra tool calls. "
        "Do not use PowerShell, Python, package managers, or newer Windows-only utilities. "
        "Use call \"file.bat\" when running a batch file before another command. "
        // file manipluation via perl
        "perl.exe is available and should always be used for creating, replacing, or editing text files. "
        "For whole-file writes, use perl.exe with open/print/close. "
        "For edits, use perl.exe -0777 for whole-file search/replace, usually with -pi.bak. "
        // internet access via curl
        "curl.exe is available for internet access. "
        // writing C code
        "When generating C to compile with cl/VC6, use C89-compatible C: declare variables at the start of a block, "
        "do not declare variables inside for loops, use int main(void), avoid // comments and avoid C99 features.",
        1
    },
    {NULL, NULL, NULL, 0}
};
const struct tool_t tools[] = {
    {
        "cmd",
        "Run one non-interactive Windows XP cmd.exe command. "
        "Use this for file operations, builds, program execution, system queries, networking, "
        "and simple automation. Do not use it for conversation.",
        run_cmd_params
    },
    {NULL, NULL, NULL}
};

char *tool_handle_run_cmd(const char *cmd) {
    char buf[1024], *out, *wrapped;
    size_t outsize = 4096, outlen = 0, buflen = 0, wrapped_size;
    FILE *f;
    
    // wrap command to capture stderr
    // TODO: maybe change to CreateProcessA for proper stderr support?
    wrapped_size = strlen(cmd) + 32;
    wrapped = malloc(wrapped_size);
    _snprintf(wrapped, wrapped_size, "cmd.exe /C %s 2>&1", cmd);
    wrapped[wrapped_size - 1] = '\0';

    f = _popen(wrapped, "r");
    free(wrapped);
    if (!f) return NULL;

    out = malloc(outsize);
    out[0] = '\0';
    
    while (fgets(buf, sizeof(buf), f)) {
        buflen = strlen(buf);
        if (outsize  < outlen + buflen + 1) {
            outsize *= 2;
            out = realloc(out, outsize);
         }
         memcpy(out + outlen, buf, buflen + 1);
         outlen += buflen;
    }
    
    _pclose(f);
    
    return out;
}

char *tool_dispatch(const char *name, const char *args_json) {
    const char *cmd;
    char *out;
    cJSON *args = cJSON_Parse(args_json);

    term_color(C_DARK_GRAY);
    printf("  $ %s (", name);

    if (!strcmp(name, "cmd")) {
        cmd = cJSON_GetObjectItemCaseSensitive(args, "command")->valuestring;

        dbg("Tool call (%s): %s\n", name, cmd);

        if (strlen(cmd) > 62) printf("%.62s...", cmd);
        else fputs(cmd, stdout);
        out = tool_handle_run_cmd(cmd);
    }

    printf(")\n");
    term_reset();

    cJSON_Delete(args);
    return out;
}

//////////////////////////////////////////////////////////////////////////
//
// CONVERSATION HANDLING

struct conversation_t {
    cJSON *root;
    cJSON *messages;

    const char *content; // last content
    cJSON *pending_tool_msg; // message with pending tool calls
};

void convo_init(struct conversation_t *convo, const char *model, const struct tool_t *tooldefs) {
    const struct tool_t *tooldef;
    const struct tool_params_t *paramdef;
    cJSON *messages;
    cJSON *tools, *tool, *func, *params, *props, *prop, *reqs;
    cJSON *root = cJSON_CreateObject();
    int i, j;

    tools = cJSON_CreateArray();

    printf("  available tools: ");
    for (i = 0; tooldefs[i].name; ++i) {
        tooldef = &tooldefs[i];

        tool = cJSON_CreateObject();
        func = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddItemToObject(tool, "function", func);

        params = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", tooldef->name);
        cJSON_AddStringToObject(func, "description", tooldef->desc);
        cJSON_AddItemToObject(func, "parameters", params);

        cJSON_AddStringToObject(params, "type", "object");
        props = cJSON_CreateObject();
        reqs = cJSON_CreateArray();

        for (j = 0; tooldef->params[j].name; ++j) {
            paramdef = &tooldef->params[j];

            prop = cJSON_CreateObject();
            cJSON_AddStringToObject(prop, "type", paramdef->type);
            cJSON_AddStringToObject(prop, "description", paramdef->desc);

            cJSON_AddItemToObject(props, paramdef->name, prop);
            if (paramdef->required) cJSON_AddItemToArray(reqs, cJSON_CreateString(paramdef->name));
        }
        cJSON_AddItemToObject(params, "properties", props);
        cJSON_AddItemToObject(params, "required", reqs);

        cJSON_AddItemToArray(tools, tool);

        fputs(tooldef->name, stdout);
    }
    printf("\n\n");

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddItemToObject(root, "tools", tools);

    messages = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "messages", messages);

    convo->root = root;
    convo->messages = messages;
    convo->content = NULL;
    convo->pending_tool_msg = NULL;
}

void convo_clear(struct conversation_t *convo) {
    cJSON *messages, *system;

    system = cJSON_DetachItemFromArray(convo->messages, 0);
    cJSON_DeleteItemFromObject(convo->root, "messages");
    
    messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToObject(convo->root, "messages", messages);

    convo->messages = messages;
    convo->content = NULL;
    convo->pending_tool_msg = NULL;
}

void convo_add_text_message(struct conversation_t *convo, const char *role, const char *content_str) {
    cJSON *msg, *content;
    
    msg = cJSON_CreateObject();
    content = cJSON_CreateString(content_str);
    
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddItemToObject(msg, "content", content);
    
    cJSON_AddItemToArray(convo->messages, msg);

    convo->content = content->valuestring;
}

void convo_add_tool_message(struct conversation_t *convo, const char *id, const char *content_str) {
    cJSON *msg, *content;
    
    msg = cJSON_CreateObject();
    content = cJSON_CreateString(content_str);
    
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", id);
    cJSON_AddItemToObject(msg, "content", content);

    cJSON_AddItemToArray(convo->messages, msg);
    
    convo->content = content->valuestring;
}

int convo_add_response(struct conversation_t *convo, const char *json) {
    cJSON *res, *choice, *msg;
    cJSON *role, *content;
    const char *finish_reason;
    int has_tools = 0;
    
    res = cJSON_Parse(json);

    dbg("Response: %s\n", cJSON_Print(res));

    choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(res, "choices"), 0);
    finish_reason = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason")->valuestring;
    msg = cJSON_GetObjectItemCaseSensitive(choice, "message");
    role = cJSON_GetObjectItemCaseSensitive(msg, "role");

    if (!strcmp(finish_reason, "tool_calls")) {
        msg = cJSON_Duplicate(msg, cJSON_True);

        // strip reasoning traces and refusals -> keep requests smaller
        cJSON_DeleteItemFromObject(msg, "reasoning");
        cJSON_DeleteItemFromObject(msg, "reasoning_details");
        cJSON_DeleteItemFromObject(msg, "refusal");

        cJSON_AddItemToArray(convo->messages, msg);

        convo->pending_tool_msg = msg;
        has_tools = 1;
    } else {
        content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        // fine to not duplicate these strings as convo_add_text_message copies
        convo_add_text_message(convo, role->valuestring, content->valuestring);
    }

    cJSON_Delete(res);

    return has_tools;
}

void convo_handle_tool_calls(struct conversation_t *convo) {
    cJSON *tcs, *tc, *func;
    const char *id, *name, *args_json;
    char *content;

    if (!convo->pending_tool_msg) return;

    tcs = cJSON_GetObjectItemCaseSensitive(convo->pending_tool_msg, "tool_calls");
    cJSON_ArrayForEach(tc, tcs) {
        func = cJSON_GetObjectItemCaseSensitive(tc, "function");

        id = cJSON_GetObjectItemCaseSensitive(tc, "id")->valuestring;
        name = cJSON_GetObjectItemCaseSensitive(func, "name")->valuestring;
        args_json = cJSON_GetObjectItemCaseSensitive(func, "arguments")->valuestring;
 
        content = tool_dispatch(name, args_json);
        convo_add_tool_message(convo, id, content);
        free(content);
    }

    convo->pending_tool_msg = NULL;
}

void convo_cleanup(struct conversation_t *convo) {
    if (convo->root) cJSON_Delete(convo->root);
    convo->root = NULL;
    convo->messages = NULL;
    convo->content = NULL;
    convo->pending_tool_msg = NULL;
}

//////////////////////////////////////////////////////////////////////////
//
// OPENROUTER INTERACTIONS

void openrouter_init(struct http_context_t *ctx, const char *api_key) {
    char buf[255];
    
    _snprintf(buf, sizeof(buf), "Authorization: Bearer %s", api_key);
    ctx->headers = curl_slist_append(ctx->headers, buf);
    ctx->headers = curl_slist_append(ctx->headers, "Content-Type: application/json");
    
    curl_easy_setopt(ctx->curl, CURLOPT_URL, "https://openrouter.ai/api/v1/chat/completions");
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);
}

void openrouter_request(struct http_context_t *ctx, struct conversation_t *convo) {
    CURLcode res;
    char *payload = cJSON_PrintUnformatted(convo->root);

    dbg("Request: %s\n", cJSON_Print(convo->root));

    ctx->reslen = 0;
    if (ctx->res) ctx->res[0] = '\0';

    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, payload);
    res = curl_easy_perform(ctx->curl);
    free(payload);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }
}

//////////////////////////////////////////////////////////////////////////
//
// STRING UTILS

char *ltrim(char *str) {
    while (isspace(*str)) ++str;
    return str;
}

char *rtrim(char *str) {
    char *last = str + strlen(str);
    while (last > str && isspace(*(last-1))) --last;
    *last = '\0';
    return str;
}

char *trim(char *str) {
    return ltrim(rtrim(str));
}

/* training:

write me C89 code that prints a xmas tree, compile it with cl and run it. save it to tree.c
*/

//////////////////////////////////////////////////////////////////////////
//
// MAIN AGENT LOOP

const char *system_prompt =
    "You are a helpful AI agent running inside Windows XP cmd.exe. "
    "Use British English and plain ASCII only: no Unicode, smart quotes, emojis, or Markdown. "
    "Use normal sentence capitalisation; use uppercase only for headings. "
    "Format lists with hyphens and tables with ASCII characters only: +, -, and |. "
    "Recommend only commands and approaches that work in this Windows XP cmd.exe environment. "
    "Be brief, practical, and return only the direct answer.";

int main(int argc, char **argv) {
    char prompt_buf[2048], *prompt, *out;
    struct config_t config;
    struct http_context_t http;
    struct conversation_t convo;

    term_init();
    config_init(&config, ini_file);

    printf("vc6ai\n\n");

    term_color(C_DARK_GRAY);
    printf("  available commands: /new, /exit\n");

    http_init(&http, 30L);
    openrouter_init(&http, config.openrouter_api_key);

    convo_init(&convo, config.openrouter_model, tools);
    convo_add_text_message(&convo, "system", system_prompt);

    term_reset();

    printf("> ");

    term_color(C_TURQUOISE);
    while (fgets(prompt_buf, sizeof(prompt_buf), stdin)) {
        prompt = trim(prompt_buf);
        if (strlen(prompt) == 0) goto prompt;

        putchar('\n');

        if (prompt[0] == '/') {
            // handle slash commands
            if (!strncmp(&prompt[1], "exit", 4)) {
                goto cleanup;
            } else if (!strncmp(&prompt[1], "new", 3)) {
                term_reset();
                printf("  ! conversation cleared\n\n");
                convo_clear(&convo);
                goto prompt;
            }

            term_color(C_RED);
            fprintf(stderr, "unrecognised slash command: %s\n", prompt);
            goto prompt;
        } else if (prompt[0] == '!') {
            // execute cmd directly
            out = tool_handle_run_cmd(&prompt[1]);
            if (strlen(out)) {
                term_reset();
                printf("%s\n\n", out);
            }
            free(out);
            goto prompt;
        }

        // store user message
        convo_add_text_message(&convo, "user", prompt);

        term_reset();
        printf("  ~ thinking ...\n");

request:
        // send user message to LLM
        openrouter_request(&http, &convo);

        // parse LLM (assistant) response, check if needs tools
        // TODO: handle if http.res is error
        if (convo_add_response(&convo, http.res)) {
            convo_handle_tool_calls(&convo);
            // send tool call result to LLM
            goto request;
        }

        // print LLM response
        term_color(C_WHITE);
        printf("\n%s\n\n", convo.content);

prompt:
        term_reset();
        printf("> ");
        term_color(C_TURQUOISE);
    }

cleanup:
    convo_cleanup(&convo);
    http_cleanup(&http);
    term_reset();

    return EXIT_SUCCESS;
}
