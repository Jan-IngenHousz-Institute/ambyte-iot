#include <cstring>
#include <cstdint>
inline size_t strlcpy(char* d,const char* s,size_t n){size_t z=strlen(s);if(n){size_t k=z<n-1?z:n-1;memcpy(d,s,k);d[k]=0;}return z;}
