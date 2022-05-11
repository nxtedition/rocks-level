
{
  "variables": { "openssl_fips": "0" },
  "targets": [
    {
      "target_name": "zstd",
      "type": "static_library",
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "zstd/lib",
        "zstd/lib/common"
      ],
      "direct_dependent_settings": { 
        "include_dirs": ["zstd/lib/"] 
      },
      "sources": [
        "zstd/lib/compress/zstd_compress_superblock.c",
        "zstd/lib/compress/zstdmt_compress.c",
        "zstd/lib/compress/zstd_double_fast.c",
        "zstd/lib/compress/zstd_fast.c",
        "zstd/lib/compress/zstd_compress_sequences.c",
        "zstd/lib/compress/zstd_ldm.c",
        "zstd/lib/compress/hist.c",
        "zstd/lib/compress/zstd_compress.c",
        "zstd/lib/compress/zstd_lazy.c",
        "zstd/lib/compress/zstd_compress_literals.c",
        "zstd/lib/compress/huf_compress.c",
        "zstd/lib/compress/zstd_opt.c",
        "zstd/lib/compress/fse_compress.c",
        "zstd/lib/dictBuilder/cover.c",
        "zstd/lib/dictBuilder/divsufsort.c",
        "zstd/lib/dictBuilder/fastcover.c",
        "zstd/lib/dictBuilder/zdict.c",
        "zstd/lib/decompress/zstd_ddict.c",
        "zstd/lib/decompress/huf_decompress.c",
        "zstd/lib/decompress/zstd_decompress.c",
        "zstd/lib/decompress/zstd_decompress_block.c",
        "zstd/lib/common/entropy_common.c",
        "zstd/lib/common/fse_decompress.c",
        "zstd/lib/common/debug.c",
        "zstd/lib/common/xxhash.c",
        "zstd/lib/common/pool.c",
        "zstd/lib/common/threading.c",
        "zstd/lib/common/zstd_common.c",
        "zstd/lib/common/error_private.c",
        "zstd/lib/deprecated/zbuff_common.c",
        "zstd/lib/deprecated/zbuff_decompress.c",
        "zstd/lib/deprecated/zbuff_compress.c"
      ],
      "conditions": [
        [
          "OS == 'mac'", {
            "xcode_settings": {
              "OTHER_CPLUSPLUSFLAGS": [
                "-mmacosx-version-min=10.15",
                "-arch x86_64",
                "-arch arm64"
              ],
              "GCC_ENABLE_CPP_RTTI": "YES",
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "MACOSX_DEPLOYMENT_TARGET": "10.15"
            },
          }
        ],
        [
          "OS == 'linux'", {
            "sources": [
              "zstd/lib/decompress/huf_decompress_amd64.S",
            ]
          }
        ]
      ]
    }
  ]
}