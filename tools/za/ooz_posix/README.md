# ooz on Linux

`third_party/ooz` is Powzix's decompressor kept unchanged (see its `README.ZARecomp.md`), and it is written for MSVC.
These headers stand in for the Windows ones it includes, so the same files build as `libooz.so` for the Linux
zaprompts helper (`tools/za/dist-linux.sh`):

    g++ -O2 -shared -fPIC -Itools/za/ooz_posix -include tools/za/ooz_posix/ooz_posix.h -include sys/stat.h \
        -Dmain=ooz_main third_party/ooz/{kraken,bitknit,lzna,ooz_lib}.cpp -o build/ooz/libooz.so

GPLv3 like ooz itself (`third_party/ooz/COPYING`): these files only exist to build it.
