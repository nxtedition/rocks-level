{
  "variables": { "openssl_fips": "0" },
  "targets": [
    {
      "target_name": "leveldown",
      "conditions": [
        [
          "OS == 'win'",
          {
            "defines": ["_HAS_EXCEPTIONS=1", "OS_WIN=1"],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "RuntimeTypeInfo": "false",
                "EnableFunctionLevelLinking": "true",
                "ExceptionHandling": "2",
                "DisableSpecificWarnings": [
                  "4355",
                  "4530",
                  "4267",
                  "4244",
                  "4506"
                ]
              },
              "VCLinkerTool": {
                "AdditionalDependencies": ["Shlwapi.lib", "rpcrt4.lib"]
              }
            }
          },
          {
            "cflags": ["-std=c++20"],
            "cflags!": ["-fno-rtti"],
            "cflags_cc!": ["-fno-rtti"],
            "cflags_cc+": ["-frtti"]
          }
        ],
        [
          "OS == 'linux'",
          {
            "cflags": [
              "-msse4.2",
              "-mpclmul",
              "-mavx",
              "-mavx2",
              "-mbmi",
              "-mlzcnt"
            ],
            "ccflags": ["-flto"],
            "cflags!": ["-fno-exceptions"],
            "cflags_cc!": ["-fno-exceptions"],
            "ldflags": ["-flto", "-fuse-linker-plugin"]
          }
        ],
        [
          "OS == 'mac'",
          {
            "xcode_settings": {
              "WARNING_CFLAGS": [
                "-Wno-sign-compare",
                "-Wno-unused-variable",
                "-Wno-unused-function",
                "-Wno-ignored-qualifiers"
              ],
              "OTHER_CPLUSPLUSFLAGS": [
                "-mmacosx-version-min=10.15",
                "-std=c++20",
                "-fno-omit-frame-pointer",
                "-momit-leaf-frame-pointer",
                "-arch x86_64",
                "-arch arm64"
              ],
              "GCC_ENABLE_CPP_RTTI": "YES",
              "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
              "MACOSX_DEPLOYMENT_TARGET": "10.15"
            }
          }
        ]
      ],
      "dependencies": ["<(module_root_dir)/deps/rocksdb/rocksdb.gyp:rocksdb"],
      "include_dirs": ["<!(node -e \"require('napi-macros')\")"],
      "sources": ["binding.cc"]
    }
  ]
}
