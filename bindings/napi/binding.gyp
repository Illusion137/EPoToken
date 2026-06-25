{
  "targets": [
    {
      "target_name": "epotoken_napi",
      "sources": [
        "epotoken_napi.cpp",
        "../../src/base64.cpp",
        "../../src/http_client.cpp",
        "../../src/challenge.cpp",
        "../../src/innertube_client.cpp",
        "../../src/placeholder.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "../../include",
        "../../src"
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
