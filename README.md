# mod_fillin [AHTSE](https://github.com/lucianpls/AHTSE)

An AHTSE module that uses lower resolution tiles from the source to 
generate output tiles which are not in the source. The source should 
be a sparse tile service. Missing tiles are recognized either by the 
HTTP Not Available (404) error, or by matching the source ETag.  

## Limitatons
Only 8 bit JPEG input and output are currently supported

## Apache configuration directives:

**Fill_RegExp pattern**  
Can be present more than once, one of the patterns has to match the request URL

**Fill_ConfigurationFiles SourceConfig Config**  
The first parameter is the source raster configuration, second one is the ahtse_fill configuration

**Fill_Source path**  
Source for input tiles, an http internal redirect path

**Fill_BackFill On**  
Optional, sends the lower request directly to the source, not to itself. Useful when this module is 
configured after (on top of) the service to be filled in

**Fill_SoCache socache_definition**  
Optional, use the socache defined by the pattern to cache input tiles. This is useful when the source is slow, 
because the inputs are usually needed for at least four output adjacent tiles. The socache_definition string depends 
on which socache module is used.

**Fill_SoCacheHints key_size tile_size timeout_in_us**  
Optional, size hints for socache if defined. Defaults to key_size=64, tile_size=65536, timeout_in_us=600000000 (10 minutes)

## Directives in both SourceConfig and Config
Normally the source and output configuration are identical

**Size X Y Z C**  
Mandatory, size of full resolution raster in pixels, MRF style

**PageSize X Y 1 C**  
Optional, pagesize in pixels, defaults to interleaved 512x512

**DataType type**  
If present, only Byte is valid

**SkippedLevels N**  
Optional, defaults to 0, counted from the top of the MRF pyramid

## Directives in the SourceConfig only
**ETagSeed base32_value**  
A 64bit number formatted as a 13 base32 digits [0-9a-v]. In the input file, this exact value will be recognized as the 
missing tile, in addition to the HTTP NOT AVAILABLE (404) response

## Directives in the output configuration only

**Quality N**
JPEG compression quality, between 0 and 99

**Nearest On**
If set, oversampling gets done using the nearest input value, which may lead to pixelization
The default oversampling is Nearest followed by a blur

**BlurStrength N**
If not in nearest mode, the value controls the strength of the blur. A value between 0 and 10, where 10 is the strongest and 0 the weakest.

## Use

Normally, the SourceConfig and the Config file are the same file. In this case, the module will insure that all output tiles are generated up to 
the full resolution, regardless of how the source behaves

## Notes

Avoid recursion internal server error by increasing the limit from the default value of 10. A value of 25 should be plenty in most cases:  
`LimitInternalRecursion 25`
