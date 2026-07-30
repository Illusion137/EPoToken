{
  "targets": [
    {
      "target_name": "epotoken_napi",
      "sources": [
        "epotoken_napi.cpp",
        "cpp/core/src/base64.cpp",
        "cpp/core/src/http_client.cpp",
        "cpp/core/src/challenge.cpp",
        "cpp/core/src/innertube_messages.cpp",
        "cpp/core/src/innertube_client.cpp",
        "cpp/core/src/placeholder.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "cpp/core/include",
        "cpp/core/src"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [
        "-std=c++20",
        "-fexceptions"
      ],
      "cflags_cc!": [
        "-fno-exceptions"
      ],
      "conditions": [
        [
          "OS=='mac'",
          {
            "libraries": [
              "-lcurl"
            ],
            "xcode_settings": {
              "OTHER_CPLUSPLUSFLAGS": [
                "-std=c++20"
              ],
              "CLANG_CXX_LANGUAGE_STANDARD": "c++20"
            }
          }
        ],
        [
          "OS=='linux'",
          {
            "libraries": [
              "-lcurl"
            ]
          }
        ],
        [
          "OS=='win'",
          {
            "libraries": [
              "libcurl.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "AdditionalOptions": [
                  "/std:c++20"
                ]
              }
            }
          }
        ]
      ]
    }
  ]
}
