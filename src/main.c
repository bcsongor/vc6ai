#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <windows.h>

#include <curl/curl.h>
#include "cJSON.h"

#pragma comment(lib, "libcurl")

#define DEBUG_REQUESTS
#define DEBUG_TOOLS

//////////////////////////////////////////////////////////////////////////
//
// DEBUGGING

#ifdef DEBUG_REQUESTS
static FILE *fdbg_reqs;
#define DBG_REQ(fmt, a) dbg(&fdbg_reqs, "requests.log", fmt, a)
#endif

#ifdef DEBUG_TOOLS
static FILE *fdbg_tools;
#define DBG_TOOL(fmt, a, b) dbg(&fdbg_tools, "tool_calls.log", fmt, a, b)
#endif

#if defined(DEBUG_REQUESTS) || defined(DEBUG_TOOLS)
static void dbg(FILE **fp, const char *filename, const char *fmt, ...) {
    va_list args;

    if (!*fp) *fp = fopen(filename, "a");
    
    va_start(args, fmt);
    vfprintf(*fp, fmt, args);
    va_end(args);

    fprintf(*fp, "-------------------------------------------------------------------------------\n");
    fflush(*fp);
}
#endif

//////////////////////////////////////////////////////////////////////////
//
// CONFIGURATION

#define APIKEY_MAX 128
#define MODEL_MAX 255

struct config_t {
    char cwd[MAX_PATH];

    // OpenRouter config
    char api_key[APIKEY_MAX];
    char model[MODEL_MAX];
    char provider[255];
    char effort[12]; // max, xhigh, high, medium, low, minimial, none
    int datacoll;
    int zdr;

    // Exa config
    char exa_key[APIKEY_MAX];

    int context; // model's context window
};

const char *ini_file = "vc6ai.ini";

void config_init(struct config_t *config, const char *ini_file) {
    char ini_path[MAX_PATH];
    
    GetCurrentDirectoryA(MAX_PATH, config->cwd);
    _snprintf(ini_path, MAX_PATH, "%s\\%s", config->cwd, ini_file);

    GetPrivateProfileStringA("OpenRouter", "ApiKey", "", config->api_key, APIKEY_MAX, ini_path);
    GetPrivateProfileStringA("OpenRouter", "Model", "deepseek/deepseek-v4-flash", config->model, MODEL_MAX, ini_path);
    GetPrivateProfileStringA("OpenRouter", "Provider", "", config->provider, sizeof(config->provider), ini_path);
    GetPrivateProfileStringA("OpenRouter", "Effort", "none", config->effort, sizeof(config->effort), ini_path);
    config->datacoll = GetPrivateProfileIntA("OpenRouter", "DataCollection", 0, ini_path);
    config->zdr = GetPrivateProfileIntA("OpenRouter", "ZeroDataRetention", 0, ini_path);
    GetPrivateProfileStringA("Exa", "ApiKey", "", config->exa_key, APIKEY_MAX, ini_path);
    if (config->exa_key[0]) SetEnvironmentVariableA("EXA_API_KEY", config->exa_key);
    config->context = 0;
}

//////////////////////////////////////////////////////////////////////////
//
// TERMINAL

enum term_color {
    // foreground
    C_FG_DARK_CYAN   = FOREGROUND_BLUE | FOREGROUND_GREEN,
    C_FG_DARK_YELLOW = FOREGROUND_RED | FOREGROUND_GREEN,
    C_FG_DARK_GRAY   = FOREGROUND_INTENSITY,

    C_FG_GRAY        = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN,
    C_FG_BLUE        = FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_FG_TURQUOISE   = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_FG_RED         = FOREGROUND_RED | FOREGROUND_INTENSITY,
    C_FG_PURPLE      = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_FG_YELLOW      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_FG_WHITE       = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,

    // background
    C_BG_DARK_BLUE   = BACKGROUND_BLUE,
    C_BG_DARK_RED    = BACKGROUND_RED,
    C_BG_DARK_PURPLE = BACKGROUND_RED | BACKGROUND_BLUE
};

enum term_md_state {
    MD_NORMAL,
    MD_BOLD,
    MD_ITALIC,
    MD_CODE_SPAN,
    MD_LINK_TEXT,
    MD_LINK_URL,
    MD_BLOCKQUOTE,
    MD_CODE_BLOCK
};

static HANDLE term_handle;
static WORD term_orig_attrs;

static volatile LONG term_interrupted_flag = 0;

static BOOL WINAPI term_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        term_interrupted_flag = 1;
        return TRUE;
    }
    return FALSE;
}

void term_init(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    term_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(term_handle, &csbi);
    term_orig_attrs = csbi.wAttributes;

    SetConsoleCtrlHandler(term_ctrl_handler, TRUE);
}

int term_interrupted(void) {
    return term_interrupted_flag != 0;
}

void term_interrupt_reset(void) {
    term_interrupted_flag = 0;
}

void term_color(enum term_color color) {
    SetConsoleTextAttribute(term_handle, color);
};

void term_reset(void) {
    SetConsoleTextAttribute(term_handle, term_orig_attrs);
}

void term_render_markdown(const char *str) {
    size_t i, len = strlen(str);
    enum term_md_state state = MD_NORMAL;
    char c;
#define BOL (i == 0 || str[i-1] == '\n')
#define TBK (i + 2 < len && str[i+1] == '`' && str[i+2] == '`')

    term_reset();
    for (i = 0; i < len; ++i) {
        c = str[i];

        // passthrough states
        if (state == MD_CODE_BLOCK) {
            if (c == '`' && BOL && TBK) { state = MD_NORMAL; term_reset(); i += 2; continue; }
            putchar(c); continue;
        }
        if (state == MD_CODE_SPAN) { if (c == '`') { state = MD_NORMAL; term_reset(); } else putchar(c); continue; }
        if (state == MD_LINK_URL) { putchar(c); if (c == ')') { state = MD_NORMAL; term_reset(); } continue; }
        if (state == MD_BLOCKQUOTE) { if (c == '\n') { state = MD_NORMAL; term_reset(); } putchar(c); continue; }
        if (state == MD_LINK_TEXT) {
            if (c == ']' && i + 1 < len && str[i+1] == '(') {
                putchar(c); state = MD_LINK_URL; i++; term_color(C_FG_DARK_GRAY); putchar('(');
            } else putchar(c);
            continue;
        }

        // heading
        if (c == '#' && BOL) {
            int level = 1;
            while (i + 1 < len && str[i+1] == '#') { level++; i++; }
            while (i + 1 < len && str[i+1] == ' ') i++;
            term_color(C_FG_WHITE | (level == 1 ? C_BG_DARK_RED : level == 2 ? C_BG_DARK_PURPLE : C_BG_DARK_BLUE));
            putchar(' ');
            continue;

        }

        // code block / span
        if (c == '`' && BOL && TBK) { state = MD_CODE_BLOCK; term_color(C_FG_YELLOW); i += 2; while (i + 1 < len && str[i+1] != '\n') i++; continue; }
        if (c == '`') { state = MD_CODE_SPAN; term_color(C_FG_YELLOW); continue; }
        
        // bold or italic, * or _, doubled or tripled for bold
        if (c == '*' || c == '_') {
            int n = 1;
            while (i + n < len && str[i+n] == c && n < 3) n++;
            if (c == '_' && i > 0 && isalnum(str[i-1]) && i + n < len && isalnum(str[i+n])) { putchar(c); continue; } // skip if snake_case_name
            if (n >= 2) {
                if (state == MD_BOLD) { state = MD_NORMAL; term_reset(); } else { state = MD_BOLD; term_color(C_FG_WHITE); }
            } else {
                if (state == MD_ITALIC) { state = MD_NORMAL; term_reset(); } else { state = MD_ITALIC; term_color(C_FG_DARK_YELLOW); }
            }
            i += n - 1; continue;
        }

        // list: - at line start
        if (c == '-' && i + 1 < len && str[i+1] == ' ' && BOL) { putchar(249); putchar(' '); i++; continue; } // replace - with bullet

        // blockquote: > at line start, followed by space or end of line. grey until newline resets
        if (c == '>' && BOL && (i + 1 >= len || str[i+1] == ' ' || str[i+1] == '\n')) {
            state = MD_BLOCKQUOTE; term_color(C_FG_DARK_GRAY); putchar('>');
            continue;
        }

        // link: only treat [ as a link when ]( follows on the same line
        if (c == '[') {
            size_t j = i + 1;
            while (j < len && str[j] != '\n' && str[j] != ']') j++;
            if (j + 1 < len && str[j] == ']' && str[j+1] == '(') {
                state = MD_LINK_TEXT; term_color(C_FG_BLUE); putchar(c); continue;
            }
        }

        if (c == '\n') term_reset();
        putchar(c);
    }
#undef BOL
#undef TBK
    term_reset();
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

static int http_progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    return term_interrupted();
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
    curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(ctx->curl, CURLOPT_PROGRESSFUNCTION, http_progress_callback);

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
        "Command passed to Windows XP cmd.exe /C, limited to about 4000 characters. "
        "Use one command per call and join dependent steps with &&; run independent calls in parallel. "
        "Use call \"file.bat\" when running a batch file before another command. "
        "Use only Windows XP-compatible tools; do not use PowerShell, Python, Perl, package managers, or newer Windows commands. "
        "busybox.exe provides sh, find, grep, sed, awk, cat, head, tail, diff, patch, tee, and unix2dos. "
        "Use forward-slash paths and short POSIX options with BusyBox; GNU long options such as --include are unsupported. Scope searches and cap their output. "
        "For pipes, regexes, or difficult quoting, run busybox.exe sh and pass the script in stdin. "
        "For edits, pass a unified diff to busybox.exe patch -p1 in stdin and verify the result; malformed hunks can exit successfully without applying. "
        "Do not retry a failed patch unchanged or convert CRLF before patching. "
        "patch, tee, and sed -i write LF, so restore existing CRLF files with unix2dos. "
        "Use tee and stdin for whole-file writes. "
        "jq.exe is available for parsing and transforming JSON; prefer it to grep or sed for JSON; use jq -r when plain-text output is needed. "
        "curl.exe is available for networking. If EXA_API_KEY is set, use Exa to search and fetch web pages. "
        "Post JSON on stdin to https://api.exa.ai/search or https://api.exa.ai/contents with curl.exe -s, Content-Type and x-api-key headers, and -d @-. "
        "Use /search to discover pages; nest highlights or text under contents. Use /contents to fetch known URLs; highlights and text are top-level. "
        "Prefer highlights for lookups, or text with maxCharacters for deeper reading. Set maxAgeHours to 0 only when fresh content matters.",
        1
    },
    {
        "stdin",
        "string",
        "Optional verbatim standard input. Use it for shell scripts, patches, JSON, and whole-file writes instead of command-line quoting.",
        0
    },
    {NULL, NULL, NULL, 0}
};
const struct tool_t tools[] = {
    {
        "cmd",
        "Run one non-interactive command with Windows XP cmd.exe /C. "
        "Stdout and stderr are captured and [exit N] is appended. "
        "Output over about 32 KB is trimmed; the full output is saved under C:\\Temp\\vc6ai. "
        "There is no timeout, so commands must not wait for input.",
        run_cmd_params
    },
    {NULL, NULL, NULL}
};

char *tool_handle_run_cmd(const char *cwd, const char *cmd, const char *input) {
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL, in_rd = NULL, in_wr = NULL;
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    char *cmdline, buf[1024], *out;
    size_t outsize = 4096, outlen = 0;
    DWORD readlen, exitcode, written, in_len, in_pos;
    BOOL ok;

    ok = CreatePipe(&rd, &wr, &sa, 0);
    if (!ok) return strdup("CreatePipe stdout failed\n");

    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    if (input) {
        ok = CreatePipe(&in_rd, &in_wr, &sa, 0);
        if (!ok) {
            CloseHandle(rd);
            CloseHandle(wr);
            return strdup("CreatePipe stdin failed\n");
        }
        
        SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    }

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = input ? in_rd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;

    cmdline = malloc(strlen(cmd) + 16);
    sprintf(cmdline, "cmd.exe /C %s", cmd);

    ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    free(cmdline);
    CloseHandle(wr);
    if (!ok) {
        sprintf(buf, "CreateProcessA failed: %d\n", GetLastError());
        if (input) CloseHandle(in_wr);
        CloseHandle(rd);
        return strdup(buf);
    }

    // send input
    if (input) {
        in_len = (DWORD)strlen(input);
        in_pos = 0;

        while (in_pos < in_len) {
            ok = WriteFile(in_wr, input + in_pos, in_len - in_pos, &written, NULL);
            if (!ok || !written) break;
            in_pos += written;
        }

        CloseHandle(in_wr);
    }

    // read output
    out = malloc(outsize);
    while (ReadFile(rd, buf, sizeof(buf), &readlen, NULL) && readlen > 0) {
        // always keep some room for the exit code (32 chars)
        if (outsize < outlen + readlen + 32) {
            while (outsize < outlen + readlen + 32) outsize *= 2;
            out = realloc(out, outsize);
        }
        memcpy(out + outlen, buf, readlen);
        outlen += readlen;
    }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitcode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // cap huge output: keep head + tail, save the full output to a file
#define CAP_HEAD 8192
#define CAP_TAIL 24576
#define CAP_LIMIT (CAP_HEAD + CAP_TAIL + 512)
    
    if (outlen > CAP_LIMIT) {
        static int seq = 0;
        char path[MAX_PATH], mark[MAX_PATH + 80];
        FILE *f;
        size_t mlen;

        CreateDirectoryA("C:\\Temp", NULL);
        CreateDirectoryA("C:\\Temp\\vc6ai", NULL);
        sprintf(path, "C:\\Temp\\vc6ai\\call_%u_%d.txt", GetCurrentProcessId(), ++seq);

        f = fopen(path, "wb");
        if (f) { fwrite(out, 1, outlen, f); fclose(f); }

        sprintf(mark, "\n[... %u of %u bytes omitted, full output: %s ...]\n", outlen - CAP_HEAD - CAP_TAIL, outlen, path);
        mlen = strlen(mark);
        
        memmove(out + CAP_HEAD + mlen, out + outlen - CAP_TAIL, CAP_TAIL);
        memcpy(out + CAP_HEAD, mark, mlen);
        
        outlen = CAP_HEAD + mlen + CAP_TAIL;
    }

    sprintf(out + outlen, "\n[exit %lu]", exitcode);

    return out;
}

char *tool_dispatch(struct config_t *config, const char *name, const char *args_json) {
    const char *cmd, *input = NULL;
    char *out;
    cJSON *stdin_json, *args = cJSON_Parse(args_json);

    term_color(C_FG_DARK_CYAN);
    printf("  $ %s (", name);

    if (!strcmp(name, "cmd")) {
        cmd = cJSON_GetObjectItemCaseSensitive(args, "command")->valuestring;

        stdin_json = cJSON_GetObjectItemCaseSensitive(args, "stdin");
        if (cJSON_IsString(stdin_json)) input = stdin_json->valuestring;

#ifdef DEBUG_TOOLS
        DBG_TOOL("Tool call (%s): %s\n", name, cmd);
#endif

        if (strlen(cmd) > 62) printf("%.62s...", cmd);
        else fputs(cmd, stdout);
        out = tool_handle_run_cmd(config->cwd, cmd, input);
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

    // session usage stats
    int tokens_in;
    int tokens_out;
    int tokens_cached;
    int tokens_ctx;
    int requests;
    double cost;
};

static const char *session_id(void) {
    static char id[40];
    if (!id[0]) _snprintf(id, sizeof(id), "vc6ai-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    return id;
}

void convo_init(struct conversation_t *convo, struct config_t *config, const struct tool_t *tooldefs) {
    const struct tool_t *tooldef;
    const struct tool_params_t *paramdef;
    cJSON *messages, *provider, *cache;
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

    cJSON_AddStringToObject(root, "model", config->model);
    cJSON_AddStringToObject(root, "session_id", session_id());
    cJSON_AddStringToObject(root, "prompt_cache_key", session_id());

    // enable prompt caching
    cache = cJSON_CreateObject();
    cJSON_AddStringToObject(cache, "type", "ephemeral");
    cJSON_AddItemToObject(root, "cache_control", cache);
    cJSON_AddItemToObject(root, "tools", tools);

    messages = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "messages", messages);

    // add provider section
    provider = cJSON_CreateObject();
    if (!config->datacoll) cJSON_AddStringToObject(provider, "data_collection", "deny");
    if (config->zdr) cJSON_AddBoolToObject(provider, "zdr", cJSON_True); // zero data retention
    if (strlen(config->provider)) {
        cJSON *order = cJSON_CreateArray();
        cJSON_AddItemToArray(order, cJSON_CreateString(config->provider));
        cJSON_AddItemToObject(provider, "order", order);
        cJSON_AddBoolToObject(provider, "allow_fallbacks", cJSON_False);
    }
    cJSON_AddItemToObject(root, "provider", provider);

    // enable reasoning
    if (strncmp(config->effort, "none", 4)) {
        cJSON *reasoning = cJSON_CreateObject();
        cJSON_AddStringToObject(reasoning, "effort", config->effort);
        cJSON_AddBoolToObject(reasoning, "exclude", cJSON_False); // keep traces for tool call continuity
        cJSON_AddItemToObject(root, "reasoning", reasoning);
    }

    memset(convo, 0, sizeof(struct conversation_t));
    convo->root = root;
    convo->messages = messages;
}

void convo_clear(struct conversation_t *convo) { 
    cJSON *root = convo->root, *messages, *system;

    system = cJSON_DetachItemFromArray(convo->messages, 0);
    cJSON_DeleteItemFromObject(root, "messages");
    
    messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToObject(root, "messages", messages);

    memset(convo, 0, sizeof(struct conversation_t));
    convo->root = root;
    convo->messages = messages;
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

void convo_compact(struct conversation_t *convo) {
#define TOOL_KEEP 1024
    cJSON *msg, *role, *content;
    char buf[TOOL_KEEP + 64];
    
    cJSON_ArrayForEach(msg, convo->messages) {
        role = cJSON_GetObjectItemCaseSensitive(msg, "role");
        if (!cJSON_IsString(role)) continue;

        if (!strcmp(role->valuestring, "assistant")) {
            cJSON_DeleteItemFromObject(msg, "reasoning");
            cJSON_DeleteItemFromObject(msg, "reasoning_details");
            cJSON_DeleteItemFromObject(msg, "refusal");
        } else if (!strcmp(role->valuestring, "tool")) {
            content = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (!cJSON_IsString(content)) continue;
            if (strlen(content->valuestring) <= TOOL_KEEP + 64) continue;

            lstrcpynA(buf, content->valuestring, TOOL_KEEP + 1);
            strcat(buf, "\n[... output pruned from history ...]");

            cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(buf));
        }
    }
}

int convo_add_response(struct conversation_t *convo, const char *json) {
    cJSON *res, *choice, *msg, *err, *usage, *details;
    cJSON *role, *content, *tc, *tcs;
    const char *finish_reason;
    int has_tools = 0;
    
    res = cJSON_Parse(json);

#ifdef DEBUG_REQUESTS
    {
        char *json = cJSON_Print(res);
        DBG_REQ("Response: %s\n", json);
        free(json);
    }
#endif

    err = res ? cJSON_GetObjectItemCaseSensitive(res, "error") : NULL;
    choice = res ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(res, "choices"), 0) : NULL;

    if (err || !choice) {
        msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        term_color(C_FG_RED);
        fprintf(stderr, "  ! api: %s\n\n", msg ? msg->valuestring : "bad response");
        term_reset();
        cJSON_Delete(res);
        return -1;
    }

    // accumulate session usage stats
    usage = cJSON_GetObjectItemCaseSensitive(res, "usage");
    if (usage) {
        int ti = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens")->valueint;
        int to = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens")->valueint;

        convo->tokens_in  += ti;
        convo->tokens_out += to;
        convo->tokens_ctx = ti + to;
        convo->cost += cJSON_GetObjectItemCaseSensitive(usage, "cost")->valuedouble;
        details = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
        if (details) convo->tokens_cached += cJSON_GetObjectItemCaseSensitive(details, "cached_tokens")->valueint;
        convo->requests++;
    }

    finish_reason = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason")->valuestring;
    msg = cJSON_GetObjectItemCaseSensitive(choice, "message");
    role = cJSON_GetObjectItemCaseSensitive(msg, "role");

    if (!strcmp(finish_reason, "tool_calls")) {
        msg = cJSON_Duplicate(msg, cJSON_True);

        // strip response-only fields the API never wants back
        cJSON_DeleteItemFromObject(msg, "refusal");
        cJSON_DeleteItemFromObject(msg, "annotations");
        tcs = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        cJSON_ArrayForEach(tc, tcs) cJSON_DeleteItemFromObject(tc, "index");

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

void convo_handle_tool_calls(struct conversation_t *convo, struct config_t *config) {
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
 
        content = tool_dispatch(config, name, args_json);
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
    char buf[APIKEY_MAX + 32];
    
    _snprintf(buf, sizeof(buf), "Authorization: Bearer %s", api_key);
    ctx->headers = curl_slist_append(ctx->headers, buf);
    ctx->headers = curl_slist_append(ctx->headers, "Content-Type: application/json");
    ctx->headers = curl_slist_append(ctx->headers, "HTTP-Referer: https://csxn.gr");
    ctx->headers = curl_slist_append(ctx->headers, "X-Title: VC6ai");

    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);
}

void openrouter_fetch_limits(struct http_context_t *ctx, struct config_t *config) {
    char model[MODEL_MAX], url[MODEL_MAX + 64], *p;
    CURLcode res;
    cJSON *root, *data, *endpoints, *endpoint, *length;

    ctx->reslen = 0;
    if (ctx->res) ctx->res[0] = '\0';

    // model variant suffixes like :nitro are not part of the endpoints URL
    strcpy(model, config->model);
    if ((p = strchr(model, ':')) != NULL) *p = '\0';
    _snprintf(url, sizeof(url), "https://openrouter.ai/api/v1/models/%s/endpoints", model);

    curl_easy_setopt(ctx->curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPGET, 1L);
    res = curl_easy_perform(ctx->curl);
    if (res != CURLE_OK) return; // model context stays unknown

    // get model's context window from response
    root = cJSON_Parse(ctx->res);
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    endpoints = cJSON_GetObjectItemCaseSensitive(data, "endpoints");
    endpoint = cJSON_GetArrayItem(endpoints, 0);
    length = cJSON_GetObjectItemCaseSensitive(endpoint, "context_length");
    if (cJSON_IsNumber(length)) config->context = length->valueint;
    cJSON_Delete(root);
}

CURLcode openrouter_request(struct http_context_t *ctx, struct conversation_t *convo) {
    CURLcode res;
    char *payload = cJSON_PrintUnformatted(convo->root);

#ifdef DEBUG_REQUESTS
    {
        char *json = cJSON_Print(convo->root);
        DBG_REQ("Request: %s\n", json);
        free(json);
    }
#endif

    ctx->reslen = 0;
    if (ctx->res) ctx->res[0] = '\0';

    curl_easy_setopt(ctx->curl, CURLOPT_URL, "https://openrouter.ai/api/v1/chat/completions");
    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, payload);
    res = curl_easy_perform(ctx->curl);
    free(payload);

    if (res != CURLE_OK && res != CURLE_ABORTED_BY_CALLBACK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

    return res;
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

//////////////////////////////////////////////////////////////////////////
//
// MAIN AGENT LOOP

const char *sysprompt =
    "You are VC6ai, an assistant for Windows XP cmd.exe. "
    "Use British English and plain ASCII in replies. "
    "Be concise and practical, using short paragraphs and lists only when useful. "
    "Suggest only Windows XP-compatible commands and methods. "
    "Run independent tool calls in parallel.";

int main(int argc, char **argv) {
    char buf[4096], *prompt, *out;
    struct config_t config = {0};
    struct http_context_t http = {0};
    struct conversation_t convo = {0};
    
    term_init();
    config_init(&config, ini_file);

    // fancy title with background grad
    printf("\n ");
    term_color(C_FG_WHITE | C_BG_DARK_BLUE);   printf(" VC6");
    term_color(C_FG_WHITE | C_BG_DARK_PURPLE); printf("ai");
    term_color(C_FG_WHITE | C_BG_DARK_RED);    printf(" ");
    term_reset();
    printf("\n\n");

    term_color(C_FG_DARK_GRAY);
    printf("  available commands: /model, /stats, /new, /exit\n");
    
    http_init(&http, 300L); // 5 minute timeout to allow for longer thinking sessions
    openrouter_init(&http, config.api_key);
    openrouter_fetch_limits(&http, &config);

    convo_init(&convo, &config, tools);

    // add env context to system prompt
    _snprintf(buf, sizeof(buf), "%s\nCurrent directory: %s\n", sysprompt, config.cwd);
    convo_add_text_message(&convo, "system", buf);

    for (;;) {
prompt:
        term_reset();
        printf("> ");
        term_color(C_FG_TURQUOISE);
        if (!fgets(buf, sizeof(buf), stdin)) {
            // handle Ctrl-C abort
            if (term_interrupted()) {
                term_interrupt_reset();
                clearerr(stdin);
                putchar('\n');
                goto prompt;
            }
            break;
        }
        prompt = trim(buf);
        if (strlen(prompt) == 0) goto prompt;

        putchar('\n');

        if (prompt[0] == '/') {
            // handle slash commands
            if (!strncmp(&prompt[1], "exit", 4)) {
                goto cleanup;
            } else if (!strncmp(&prompt[1], "new", 3)) {
                term_reset();
                printf("  ¯ conversation cleared\n\n");
                convo_clear(&convo);
                goto prompt;
            } else if (!strncmp(&prompt[1], "model", 5)) {
                char *arg = trim(&prompt[6]);
                if (*arg) {
                    // switch model
                    lstrcpynA(config.model, arg, MODEL_MAX);
                    cJSON_ReplaceItemInObject(convo.root, "model", cJSON_CreateString(config.model));
                    config.context = 0;
                    openrouter_fetch_limits(&http, &config);
                }
                term_reset();
                printf("  ¯ current model: %s\n", config.model);
                printf("    reasoning: %s\n", config.effort);
                if (config.context > 0) printf("    context window: %d\n", config.context);
                printf("    data collection: %s\n", config.datacoll ? "allowed" : "denied");
                printf("    zero data retention: %s\n\n", config.zdr ? "enabled" : "disabled");
                goto prompt;
            } else if (!strncmp(&prompt[1], "stats", 5)) {
                term_reset();
                printf("  ¯ session stats (%d requests)\n", convo.requests);
                printf("    tokens in:  %d\n", convo.tokens_in);
                printf("    tokens out: %d\n", convo.tokens_out);
                printf("    cached in:  %d\n", convo.tokens_cached);
                if (config.context > 0) {
                    int pct = (int)(100.0 * convo.tokens_ctx / config.context + 0.5);
                    printf("    context:    %d / %d (%d%% used)\n",
                           convo.tokens_ctx, config.context, pct);
                } else {
                    printf("    context:    %d\n", convo.tokens_ctx);
                }
                printf("    cost:       $%.4f\n\n", convo.cost);
                goto prompt;
            }

            term_color(C_FG_RED);
            fprintf(stderr, "  ¯ unrecognised slash command: %s\n\n", prompt);
            goto prompt;
        } else if (prompt[0] == '!') {
            // execute cmd directly
            out = tool_handle_run_cmd(config.cwd, &prompt[1], NULL);
            if (strlen(out)) {
                term_reset();
                printf("%s\n\n", out);
            }
            free(out);
            goto prompt;
        }

        // compact earlier turns to shring payload and save tokens:
        // - drop reasoning traces
        // - truncate old tool outputs
        // TODO: compact convo if at 80% of context
        convo_compact(&convo);

        // store user message
        convo_add_text_message(&convo, "user", prompt);

        term_color(C_FG_DARK_CYAN);
        printf("  ~ thinking ...\n");
        term_reset();

request:
        // send user message to LLM
        if (openrouter_request(&http, &convo)) {
            if (term_interrupted()) {
                term_interrupt_reset();
                printf("  ~ interrupted\n\n");
            }
            goto prompt;
        }

        // parse LLM (assistant) response
        //   -1: some API error
        //    0: ok
        //    1: ok + has tool calls 
        switch (convo_add_response(&convo, http.res)) {
            case -1: goto prompt;
            case 1:
                convo_handle_tool_calls(&convo, &config);
                // send tool call result to LLM
                goto request;
        }

        // print LLM response
        putchar('\n');
        term_render_markdown(convo.content);
        printf("\n\n");
    }

cleanup:
    convo_cleanup(&convo);
    http_cleanup(&http);
    term_reset();

    return EXIT_SUCCESS;
}
