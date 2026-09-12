`nvEncodeAPI.h` is NVIDIA's MIT-licensed NVENC API 12.1 header, distributed by
nv-codec-headers at tag `n12.1.14.0`:
https://github.com/FFmpeg/nv-codec-headers/blob/n12.1.14.0/include/ffnvcodec/nvEncodeAPI.h

SHA256: `dbdaa8bcbc2b9325bd24bc68b36170a8432411518040e8d4c8421608cf217425`.
The copyright and permission notice remain at the top of the unmodified file.
Only declarations are compiled; runtime NVENC/CUDA libraries come from the
machine's NVIDIA driver and are optional, loaded with `dlopen`.
