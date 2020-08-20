/*
* mod_ahtse_fill.cpp
* An AHTSE module that fills in the missing gaps in a tile pyramid, using data from the 
* lower resolution levels, if those are available
*
* (C) Lucian Plesea 2017
*/

#include <ahtse.h>
#include <receive_context.h>
#include <http_request.h>
#include <http_log.h>
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

    TiledRaster raster, inraster;

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

static const string normalizeETag(const char* sETag) {
    string result(sETag);
    static const char *base32chars = "0123456789abcdefghijklmnopqrstuvABCDEFGHIJKLMNOPQRSTUV";
    while (string::npos != result.find_first_not_of(base32chars))
        result.erase(result.find_first_not_of(base32chars), 1);
    while (result.size() != 13)
        result.append("0");
    return result;
}

static int handler(request_rec* r) {
    if (r->method_number != M_GET)
        return DECLINED;

    // The configuration always exists, but still
    auto cfg = get_conf<afconf>(r, &ahtse_fill_module);
    if (!cfg || !cfg->source || (cfg->indirect && r->main == nullptr))
        return DECLINED;
    if (!cfg->arr_rxp || !requestMatches(r, cfg->arr_rxp))
        return DECLINED;

    sz tile;
    if (APR_SUCCESS != getMLRC(r, tile) || tile.l >= cfg->raster.n_levels)
        return HTTP_BAD_REQUEST;

    string ETag;
    storage_manager tilebuf;
    tilebuf.size = cfg->max_input_size;
    tilebuf.buffer = static_cast<char*>(apr_palloc(r->pool, tilebuf.size));
    if (!tilebuf.buffer) {
        ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r, "Error allocating memory for input tile");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    int code = APR_SUCCESS;
    if (tile.l < cfg->inraster.n_levels) {
        // Try fetching this input tile
        char* sETag = NULL;
        code = get_remote_tile(r, cfg->source, tile, tilebuf, &sETag, cfg->suffix);
        if (code != APR_SUCCESS && code != HTTP_NOT_FOUND)
            return code;
        if (sETag)
            ETag = normalizeETag(sETag);
    }

    // If the input was fine and the etag is not the missing tile, return the image as is
    if (code == APR_SUCCESS && ETag != cfg->inraster.missing.eTag) {
        apr_table_setn(r->headers_out, "ETag", ETag.c_str());
        return sendImage(r, tilebuf);
    }

    // response was HTTP_NOT_FOUND or it was the missing tile which we ignore, this is where we fill in the tile
    return HTTP_NOT_IMPLEMENTED;
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
    const char* err_message;
    const char* line; // temporary

    apr_table_t* kvp = readAHTSEConfig(cmd->temp_pool, src, &err_message);
    if (nullptr == kvp)
        return err_message;

    err_message = configRaster(cmd->pool, kvp, c->inraster);
    if (nullptr != err_message)
        return err_message;

    // We don't really need an output configuration, but we'll take one
    kvp = readAHTSEConfig(cmd->temp_pool, fname, &err_message);
    if (nullptr == kvp)
        return err_message;
    
    err_message = configRaster(cmd->pool, kvp, c->raster);
    if (nullptr != err_message)
        return err_message;


    line = apr_table_get(kvp, "InputBufferSize");
    c->max_input_size = line ? static_cast<apr_size_t>(apr_strtoi64(line, NULL, 0))
        : DEFAULT_INPUT_SIZE;

    // Verify that rasters match
    if (c->raster.pagesize != c->inraster.pagesize)
        return "PageSize values should be identical";

    return nullptr;
}

static const command_rec cmds[] =
{
    AP_INIT_TAKE12(
        "Fill_Source",
        (cmd_func) set_source<afconf>,
        0,
        ACCESS_CONF,
        "Required, internal path for the source. Optional suffix also accepted"
    ),

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
