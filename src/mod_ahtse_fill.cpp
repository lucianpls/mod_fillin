/*
* mod_ahtse_fill.cpp
* An AHTSE module that fills in the missing gaps in a tile pyramid, using data from the 
* lower resolution levels, if those are available
*
* (C) Lucian Plesea 2017
*/

#include <ahtse.h>
// #include <apr_strings.h>
#include <unordered_map>

NS_AHTSE_USE
using namespace std;

extern module AP_MODULE_DECLARE_DATA ahtse_fill_module;

#if defined(APLOG_USE_MODULE)
APLOG_USE_MODULE(ahtse_fill);
#endif

// compressed tile is 1MB
#define DEFAULT_INPUT_SIZE (1024 * 1024)

struct afconf {
    apr_array_header_t* arr_rxp;
    // internal source path
    char* source;
    // source suffix
    char* suffix;

    apr_size_t max_input_size;
    // Set this if only indirect use is allowed
    int indirect;
};

// TODO: Move this to libahtse, remove from mod_convert also
static unordered_map<const char*, img_fmt> formats = {
    {"image/jpeg", IMG_JPEG},
    {"image/png", IMG_PNG},
    {"image / jpeg; zen=true", IMG_JPEG_ZEN}
};

static int handler(request_rec* r) {
    return DECLINED;
}

static void *create_dir_conf(apr_pool_t *p, char *path) {
    auto c = reinterpret_cast<afconf *>(apr_pcalloc(p, sizeof(afconf)));
    return c;
}

// Allow for one or more RegExp guard
// One of them has to match if the request is to be considered
static const char *set_regexp(cmd_parms *cmd, afconf *c, const char *pattern) {
    return add_regexp_to_array(cmd->pool, &c->arr_rxp, pattern);
}

static const char *read_config(cmd_parms *cmd, afconf  *c, const char *src, const char *fname) {
    return "UNIMPLEMENTED";
}

static const command_rec cmds[] =
{
    AP_INIT_TAKE2(
        "Fill_ConfigurationFiles",
        (cmd_func)read_config, // Callback
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Source and output configuration files"
    ),

    AP_INIT_TAKE1(
        "Fill_RegExp",
        (cmd_func)set_regexp,
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Regular expression that the URL has to match.  At least one is required."
    ),

    { NULL }
};

static void register_hooks(apr_pool_t *p) {
    ap_hook_handler(handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA ahtse_fill_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_conf,
    0, // No dir_merge
    0, // No server_config
    0, // No server_merge
    cmds, // configuration directives
    register_hooks // processing hooks
};
