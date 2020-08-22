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
#include <apr_md5.h>
#include <vector>

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
    int nearest;  // Defaults to blurred
    int quality;  // Defaults to 75
    int strength; // Defaults to 5

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

// Upsample by a factor of 2 one of the corners of the source page 
template <typename T>
static void oversample_NNB(T* src, T* dst, const TiledRaster &raster, int right, int bottom) {
    // Normalize
    right = right ? 1 : 0;
    bottom = bottom ? 1 : 0;
    size_t colors = raster.pagesize.c;
    size_t szx = raster.pagesize.x;
    size_t szy = raster.pagesize.y;
    size_t szl = colors * szx;
    size_t off = (right * szx / 2 + bottom * szy / 2) * szl;
    for (size_t y = 0; y < szy; y++)
        for (size_t x = 0; x < szx; x++)
            for (size_t c = 0; c < colors; c++)
                dst[y * szl + x * colors + c] =
                src[off + (y / 2) * szl + (x / 2) * colors + c];
}

// Should take a work-type for the math part
template <typename T>
static void blur(vector<T>& line, vector<T>& acc, int strength) {
    acc.clear();
    strength = 10 - strength;
    // Leave the ends as they are, blur the middle parts
    acc.push_back(line[0]);
    for (int i = 1; i < line.size() - 1 ; i++) {
        auto v = static_cast<int64_t>(line[i]) * strength + line[i + 1] + line[i - 1];
        acc.push_back(static_cast<T>(v / (strength + 2)));
    }
    acc.push_back(line.back());
    // Copy accumulator back to the input
    line.assign(acc.begin(), acc.end());
}

// Default overample, use nearest sampling, then blur it a bit
template <typename T>
static void oversample(T* src, T* dst, const TiledRaster& raster, int right, int bottom, int strength = 4) {
    oversample_NNB(src, dst, raster, right, bottom);
    size_t colors = raster.pagesize.c;
    size_t szx = raster.pagesize.x;
    size_t szy = raster.pagesize.y;
    size_t szl = colors * szx;
    // Blur by line then by column
    vector<T> acc;
    vector<T> values;
    for (int c = 0; c < colors; c++) {
        for (int y = 0; y < szy; y++) {
            values.clear();
            T* start = dst + y * szl + c;
            for (int x = 0; x < szx; x++)
                values.push_back(start[x * colors]);
            blur(values, acc, strength);
            for (int x = 0; x < szx; x++)
                start[x * colors] = values[x];
        }
    }
    for (int c = 0; c < colors; c++) {
        for (int x = 0; x < szx; x++) {
            values.clear();
            T* start = dst + x * colors + c;
            for (int y = 0; y < szy; y++)
                values.push_back(start[y * szl]);
            blur(values, acc, strength);
            for (int y = 0; y < szy; y++)
                start[y * szl] = values[y];
        }
    }
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

    if (tile.l < 0)
        return sendEmptyTile(r, cfg->raster.missing);

    // Check outside of bounds
    rset* level = cfg->raster.rsets + tile.l;
    if (tile.x < 0 || tile.y < 0 || tile.x >= level->w || tile.y >= level->h )
        return HTTP_BAD_REQUEST;

    string ETag;
    storage_manager tilebuf;
    tilebuf.size = static_cast<int>(cfg->max_input_size);
    tilebuf.buffer = static_cast<char*>(apr_palloc(r->pool, tilebuf.size));
    if (!tilebuf.buffer) {
        ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r, "Error allocating memory for input tile");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    int code = APR_SUCCESS;
    char* sETag = nullptr;
    if (tile.l < cfg->inraster.n_levels ) {
        // Try fetching this input tile
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

    // response was HTTP_NOT_FOUND or it was the missing tile
    // Request a lower level from this exact path and oversample
    sz higher_tile = tile;
    higher_tile.l--;
    higher_tile.x /= 2;
    higher_tile.y /= 2;
    if (higher_tile.l < 0)
        return sendEmptyTile(r, cfg->raster.missing);

    string new_uri(r->unparsed_uri);
    auto sloc = new_uri.find("/tile/");
    if (string::npos == sloc) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "Configuration problem, tile path should end with /tile/");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    new_uri = pMLRC(r->pool, new_uri.substr(0, sloc).c_str(), higher_tile);
    if (r->args && 0 != strlen(r->args)) {
        new_uri.append("?");
        new_uri.append(r->args);
    }

    sETag = nullptr;
    tilebuf.size = static_cast<int>(cfg->max_input_size);
    code = get_response(r, new_uri.c_str(), tilebuf, &sETag);
    if (APR_SUCCESS != code)
        return code;
    if (tilebuf.size < 4) // Should be minimum image size, 4 is too small
        return HTTP_NOT_FOUND;

    // decode, oversample and re-encode
    codec_params params;
    memset(&params, 0, sizeof(params));
    size_t pixel_size = GDTGetSize(cfg->inraster.datatype);
    size_t input_line_width = pixel_size * 
        cfg->inraster.pagesize.x * cfg->inraster.pagesize.c;
    size_t pagesize = input_line_width * cfg->inraster.pagesize.y;
    params.line_stride = static_cast<apr_uint32_t>(input_line_width);

    apr_uint32_t in_format;
    memcpy(&in_format, tilebuf.buffer, 4);

    const char* message = nullptr;

    // Double page, to hold the upsampled one also
    auto rawbuf = reinterpret_cast<unsigned char *>(apr_palloc(r->pool, 2 * pagesize));
    if (JPEG_SIG == in_format) {
        message = jpeg_stride_decode(params, cfg->inraster, tilebuf, rawbuf);
    }
    else {
        message = "Only JPEG is supported at this time";
    }

    if (message) { // Got an error from decoding
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s decoding %s", message, new_uri.c_str());
        return HTTP_NOT_FOUND;
    }

    // Got the bits, oversample the right corner
    bool right = (higher_tile.x * 2 != tile.x);
    bool bottom = (higher_tile.y * 2 != tile.y);
    if (cfg->nearest)
        oversample_NNB(rawbuf, rawbuf + pagesize, cfg->inraster, right, bottom);
    else
        oversample(rawbuf, rawbuf + pagesize, cfg->inraster, right, bottom, 0);

    // Build output tile in the tilebuf
    jpeg_params cparams;
    memset(&cparams, 0, sizeof(jpeg_params));
    cparams.line_stride = static_cast<apr_uint32_t>(input_line_width);
    cparams.quality = cfg->quality;

    storage_manager rawmgr(rawbuf + pagesize, pagesize);
    tilebuf.size = static_cast<int>(cfg->max_input_size); // Reset the image buffer
    message = jpeg_encode(cparams, cfg->raster, rawmgr, tilebuf);
    if (message) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s encoding %s", message, new_uri.c_str());
        return HTTP_NOT_FOUND;
    }

    unsigned char MDSIG[APR_MD5_DIGESTSIZE];
    apr_md5(MDSIG, tilebuf.buffer, tilebuf.size);
    apr_uint64_t val;
    memcpy(&val, MDSIG, sizeof(val));
    // Quotes are part of the syntax
    char outETag[14];
    tobase32(val, outETag);
    if (etagMatches(r, outETag))
        return HTTP_NOT_MODIFIED;
    apr_table_set(r->headers_out, "ETag", outETag);
    return sendImage(r, tilebuf);
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

    line = apr_table_get(kvp, "Nearest");
    if (line)
        c->nearest = getBool(line);

    c->strength = 5;
    line = apr_table_get(kvp, "BlurStrength");
    if (line)
        c->strength = apr_atoi64(line);
    if (c->strength > 10 || c->strength < 0)
        return "BlurStrength range is 0 to 10";

    c->quality = 75; // Default
    line = apr_table_get(kvp, "Quality");
    if (line) {
        c->quality = apr_atoi64(line);
        if (c->quality > 99 || c->quality < 0)
            return "Quality range is 0 to 99";
    }

    // Verify that rasters match
    if (c->raster.pagesize != c->inraster.pagesize)
        return "PageSize values should be identical";

    // TODO: verify that sizes are powers of two, maybe even the bounding box
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
