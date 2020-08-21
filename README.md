# mod_ahtse_fill [AHTSE](https://github.com/lucianpls/AHTSE)
## NOT FUNCTIONAL

An AHTSE module that uses lower resolution tiles to generate content where it is not available

## Notes

Avoid internal server error due to recursion, by increase the limit from the default value of 10. A value of 25 should be plenty in most cases:  
`LimitInternalRecursion 25`
