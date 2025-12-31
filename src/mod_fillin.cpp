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

// For using socache
#include <ap_provider.h>
#include <ap_socache.h>

NS_AHTSE_USE
NS_ICD_USE

using namespace std;

extern module AP_MODULE_DECLARE_DATA fillin_module;

#if defined(APLOG_USE_MODULE)
APLOG_USE_MODULE(fillin);
#endif

// compressed tile is 1MB
#define DEFAULT_INPUT_SIZE (1024 * 1024)

struct afconf {
    apr_array_header_t* arr_rxp;
    // internal source path
    char* source;
    // source suffix
    char* suffix;

    // socache info, if configured
    ap_socache_provider_t* soprovider;
    ap_socache_instance_t* soinstance;
    ap_socache_hints sohints; // keylen, objsize, expiration_interval

    TiledRaster raster, inraster;

    apr_size_t max_input_size;
    int backfill;  // On if this module does back-fill instead of pass-through fill
    int nearest;   // Defaults to blurred
    int quality;   // Defaults to 75
    int strength;  // Defaults to 5

    // Set this if only indirect use is allowed
    int indirect;
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
    size_t off = (right + bottom * szy) * szl / 2;
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
    for (size_t i = 1; i < line.size() - 1 ; i++) {
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
    // Blur by line
    vector<T> acc;
    vector<T> values;
    for (size_t c = 0; c < colors; c++) {
        for (size_t y = 0; y < szy; y++) {
            values.clear();
            T* start = dst + y * szl + c;
            for (size_t x = 0; x < szx; x++)
                values.push_back(start[x * colors]);
            blur(values, acc, strength);
            for (size_t x = 0; x < szx; x++)
                start[x * colors] = values[x];
        }
    }
    // Blur by column
    for (size_t c = 0; c < colors; c++) {
        for (size_t x = 0; x < szx; x++) {
            values.clear();
            T* start = dst + x * colors + c;
            for (size_t y = 0; y < szy; y++)
                values.push_back(start[y * szl]);
            blur(values, acc, strength);
            for (size_t y = 0; y < szy; y++)
                start[y * szl] = values[y];
        }
    }
}

static int is_redirect(int code) {
    return HTTP_MOVED_PERMANENTLY == code || HTTP_MOVED_TEMPORARILY == code;
}

// Like get_remote_tile, but follows one redirect if necessary
// TODO: Move to libahtse
static int get_remote_tile_with_redirect(request_rec* r, const char* remote, const sloc_t& tile,
    storage_manager& dst, char** psETag, const char* suffix)
{
    auto bufsize = dst.size; // In case we need to reset it
    int code = get_remote_tile(r, remote, tile, dst, psETag, suffix);
    if (is_redirect(code)) {
        // Check that sETag holds a location header. Skip the hostname, which
        // should be local
        char* sETag = *psETag;
        if (sETag && *sETag) {
            auto slash = ap_strchr(sETag, '/');
            if (slash) {
                slash = ap_strchr(slash + 1, '/');
                if (slash)
                    slash = ap_strchr(slash + 1, '/'); // Third slash
            }
            if (slash) { // Internal path only
                dst.size = bufsize; // Reset size
                // Try the second time
                code = get_response(r, slash, dst, psETag);
            }
        }
    }
    return code;
}

static int handler(request_rec* r) {
    if (r->method_number != M_GET)
        return DECLINED;

    // The configuration always exists, but still
    auto cfg = get_conf<afconf>(r, &fillin_module);
    if (!cfg || !cfg->source || (cfg->indirect && r->main == nullptr))
        return DECLINED;
    if (!cfg->arr_rxp || !requestMatches(r, cfg->arr_rxp))
        return DECLINED;

    sz5 tile;
    if (APR_SUCCESS != getMLRC(r, tile, 1) || tile.l >= static_cast<size_t>(cfg->raster.n_levels))
        return HTTP_BAD_REQUEST;

    if (tile.l < 0)
        return sendEmptyTile(r, cfg->raster.missing);

    // Check outside of bounds
    rset* level = cfg->raster.rsets + tile.l;
    if (tile.x < 0 || tile.y < 0 || tile.x >= static_cast<size_t>(level->w) || tile.y >= static_cast<size_t>(level->h))
        return HTTP_BAD_REQUEST;

    string ETag;
    storage_manager tilebuf;
    auto bufsz = cfg->max_input_size; // Save it in case of re-use

    tilebuf.size = static_cast<int>(bufsz);
    tilebuf.buffer = static_cast<char*>(apr_palloc(r->pool, tilebuf.size));
    if (!tilebuf.buffer) {
        ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0, r, "Error allocating memory for input tile");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    int code = APR_SUCCESS;
    char* sETag = nullptr;
    if (!cfg->backfill) {
        if (tile.l < static_cast<size_t>(cfg->inraster.n_levels)) {
            // Try fetching this input tile, following one redirect if necessary
            code = get_remote_tile_with_redirect(r, cfg->source, tile, tilebuf, &sETag, cfg->suffix);

            // If we still have a redirect, set the response header
            if (is_redirect(code) && sETag && *sETag)
                apr_table_setn(r->headers_out, "Location", sETag);

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
    }

    // response was HTTP_NOT_FOUND or it was the missing tile
    // Request a lower level from this exact path and oversample
    sz5 higher_tile = tile;
    higher_tile.l--;
    higher_tile.x /= 2;
    higher_tile.y /= 2;
    if (higher_tile.l < 0)
        return sendEmptyTile(r, cfg->raster.missing);

    string new_uri(r->unparsed_uri);
    if (cfg->backfill) {
        // Don't loop to itself, send the lower res request to the source
        new_uri = pMLRC(r->pool, cfg->source, higher_tile, cfg->suffix);
    }
    else {
        auto sloc = new_uri.find("/tile/");
        // This is a problem in libahtse, the getMRLC should have failed
        // So this code is just a safeguard
        if (string::npos == sloc) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                "Tile request should include /tile/");
            return HTTP_BAD_REQUEST;
        }
        new_uri = pMLRC(r->pool, new_uri.substr(0, sloc).c_str(), higher_tile);
        if (r->args && 0 != strlen(r->args)) {
            new_uri.append("?");
            new_uri.append(r->args);
        }
    }

    // If we have a socache, maybe the lower request is already there
    code = HTTP_NOT_FOUND; // Flag it as failed already
    // Build the socache key once, to make sure it is unique
    string socache_key;
    if (cfg->soinstance) {
        socache_key = pMLRC(r->pool, "", higher_tile);
        socache_key = socache_key.substr(6); // Remove "/tile/"
        apr_size_t keylen = socache_key.size();
        // socache API wants different types
        auto objbuf = reinterpret_cast<unsigned char*>(tilebuf.buffer);
        unsigned size = bufsz;
        // Returns APR_NOTFOUND if failed, SUCCESS otherwise
        code = cfg->soprovider->retrieve(cfg->soinstance, r->server, 
            (unsigned char*)socache_key.c_str(), keylen,
            (unsigned char*)tilebuf.buffer, &size, r->pool);
        if (code == APR_SUCCESS) { // Got it
            tilebuf.size = size;
            // Should we reset the expiry?
            LOG(r, "socache hit %s", socache_key.c_str());
        }
    }

    if (code != APR_SUCCESS) {
        // Not in socache, get from remote
        sETag = nullptr; // Reset etag
        tilebuf.size = bufsz;
        code = get_remote_tile_with_redirect(r, new_uri.c_str(), higher_tile, 
            tilebuf, &sETag, cfg->suffix);
        LOG(r, "Got %d response from %s", code, new_uri.c_str());
        if (code == APR_SUCCESS) {
            apr_size_t keylen = socache_key.size();
            LOG(r, "socache storing %s", socache_key.c_str());
            cfg->soprovider->store(cfg->soinstance, r->server,
                (unsigned char*)socache_key.c_str(), keylen,
                apr_time_now() + cfg->sohints.expiry_interval,
                (unsigned char*)tilebuf.buffer, (unsigned)tilebuf.size,
                r->pool);
        }
    }

    if (APR_SUCCESS != code) {
        // If it's a redirect, pass it up
        if (is_redirect(code) && sETag && *sETag)
                apr_table_setn(r->headers_out, "Location", sETag);
        return code;
    }

    if (tilebuf.size < 4) // Should be minimum image size, 4 is way too small
        return HTTP_NOT_FOUND;

    // decode, oversample and re-encode
    auto inr = cfg->inraster;
    inr.size = inr.pagesize;
    codec_params params(inr);
    params.line_stride = static_cast<apr_uint32_t>(getTypeSize(inr.dt) * inr.size.x * inr.size.c);
    size_t pagesize = inr.size.y * params.line_stride;

    apr_uint32_t in_format;
    memcpy(&in_format, tilebuf.buffer, 4);

    // Double page, to hold the upsampled one also
    auto rawbuf = reinterpret_cast<unsigned char *>(apr_palloc(r->pool, 2 * pagesize));
    const char* message = stride_decode(params, tilebuf, rawbuf);
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
    auto outr = cfg->raster;
    outr.size = outr.pagesize;
    jpeg_params cparams(outr);
    cparams.quality = cfg->quality;

    storage_manager rawmgr(rawbuf + pagesize, pagesize);
    tilebuf.size = static_cast<int>(cfg->max_input_size); // Reset the image buffer
    message = jpeg_encode(cparams, rawmgr, tilebuf);
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

static const char* set_socachehints(cmd_parms* cmd, afconf* c,
    const char* keylen, const char* objlen, const char* expiration)
{
    // Parse three numbers
    c->sohints.avg_id_len = apr_atoi64(keylen);
    c->sohints.avg_obj_size = apr_atoi64(objlen);
    c->sohints.expiry_interval = apr_atoi64(expiration);
    // Check the values for sanity
    if (c->sohints.avg_id_len > 1024)
        c->sohints.avg_id_len = 1024;
    // average tile size, between 10k and 1MB
    if (c->sohints.avg_obj_size > 1024 * 1024)
        c->sohints.avg_obj_size = 1024 * 1024;
    if (c->sohints.avg_obj_size < 10 * 1024)
        c->sohints.avg_obj_size = 10 * 1024;
    // In microseconds, under 15 minutes
    if (c->sohints.expiry_interval > 15 * 60 * 1000000)
        c->sohints.expiry_interval = 15 * 60 * 1000000;
    return nullptr; // All fine
}

static void *create_dir_conf(apr_pool_t *p, char *path) {
    auto c = reinterpret_cast<afconf *>(apr_pcalloc(p, sizeof(afconf)));
    // Set up decent hints for socache
    c->sohints.avg_id_len = 64; // 64 chars for M/L/R/C
    c->sohints.avg_obj_size = 16 * 1024; // 16K per tile
    c->sohints.expiry_interval = 5 * 60 * 1000000; // 5 minutes
    return c;
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
        c->strength = static_cast<int>(apr_atoi64(line));
    if (c->strength > 10 || c->strength < 0)
        return "BlurStrength range is 0 to 10";

    c->quality = 75; // Default
    line = apr_table_get(kvp, "Quality");
    if (line) {
        c->quality = static_cast<int>(apr_atoi64(line));
        if (c->quality > 99 || c->quality < 0)
            return "Quality range is 0 to 99";
    }

    // Verify that rasters match
    if (c->raster.pagesize != c->inraster.pagesize)
        return "PageSize values should be identical";

    // TODO: verify that sizes are powers of two, maybe even the bounding box
    return nullptr;
}

// Set socache path to use, per configuration
static const char* set_socache(cmd_parms* cmd, afconf* c, const char* socache) {
    // From the socache documentation:
    // 
    // max of 16 chars, uniquely identifies th consumer of the cache within the server
    // This string may be used within a file system path, so use only [a-z0-9_-]
    const char* CACHE_NAME = "FILLIN";
    const char* err_message;
    const char* provider_name = AP_SOCACHE_DEFAULT_PROVIDER;

    // Standard format is provider:path, otherwise it is just path
    // This is pretty fragile, if the name doesn't match the module expectation, it will crash
    // Example: shmcb:/path/to/datafile(512000)
    // In that case, it is using a memory mapped file, so the path has to be valid
    if (ap_strchr(socache, ':')) {
        provider_name = apr_pstrndup(cmd->temp_pool, socache, ap_strchr(socache, ':') - socache);
        // Pass the rest to the module itself
        socache = ap_strchr(socache, ':') + 1;
    }

    c->soprovider = (ap_socache_provider_t *)
        ap_lookup_provider(AP_SOCACHE_PROVIDER_GROUP, provider_name, AP_SOCACHE_PROVIDER_VERSION);
    if (!c->soprovider)
        return "Can't find the socache provider";
    // Got the provider, create an instance, the termination is registered with the cmd->pool
    err_message = c->soprovider->create(&c->soinstance, socache, cmd->temp_pool, cmd->pool);
    if (err_message)
        return err_message;
    // paranoia
    if (!c->soinstance)
        return "Can't create the socache session";

    // Do we initialize it here?
    // The name is max 16 chars, unique for the consumer within the server, although the session 
    // cache includes a path which can be different within the same server
    auto status = c->soprovider->init(c->soinstance, CACHE_NAME, &c->sohints, cmd->server, cmd->pool);
    if (status != APR_SUCCESS)
        return "Failed to initialize socache, possibly the hints are not suitable";

    return nullptr; // All fine
}

static const command_rec cmds[] =
{
    AP_INIT_TAKE1(
        "Fill_RegExp",
        (cmd_func)set_regexp<afconf>,
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Regular expression that the URL has to match.  At least one is required"
    ),

    AP_INIT_TAKE12(
        "Fill_Source",
        (cmd_func) set_source<afconf>,
        0,
        ACCESS_CONF,
        "Required, internal path for the source, tile/<MLRC> is added. Optional suffix is also accepted"
    ),

    AP_INIT_TAKE1(
        "Fill_BackFill",
        (cmd_func) ap_set_flag_slot,
        (void *)APR_OFFSETOF(afconf, backfill),
        ACCESS_CONF,
        "Optional, assume that the initial tile requested is missing, request the lower level directly from the source. "
        "Useful when the this module is set up after the service to be filled in"
    ),

    AP_INIT_TAKE3(
        "Fill_SoCacheHints",
        (cmd_func) set_socachehints,
        0,
        ACCESS_CONF,
        "Optional, requires and needs to be used before Fill_SoCache, takes three numerical arguments\n"
        "key_size obj_len expiration_time"
    ),
    AP_INIT_TAKE1(
        "Fill_SoCache",
        (cmd_func) set_socache,
        0,
        ACCESS_CONF,
        "Optional, socache path for caching input tiles. Format is \"provider:arg\"\n"
        ", see https://httpd.apache.org/docs/2.4/socache.html. Example: \"shmcb:/path/to/datafile(512000)\""
    ),

    AP_INIT_TAKE2(
        "Fill_ConfigurationFiles",
        (cmd_func)read_config, // Callback
        0, // Self-pass argument
        ACCESS_CONF, // availability
        "Source and output configuration files"
    ),

    { NULL }
};

static void register_hooks(apr_pool_t *p) {
    ap_hook_handler(handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA fillin_module = {
    STANDARD20_MODULE_STUFF,
    create_dir_conf,
    0, // No dir_merge
    0, // No server_config
    0, // No server_merge
    cmds, // configuration directives
    register_hooks // processing hooks
};
