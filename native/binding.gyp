{
  "targets": [
    {
      "target_name": "vst2_host",
      "sources": [
        "vst2_host.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "libraries": []
          }
        ]
      ]
    },
    {
      "target_name": "vcam_sender",
      "sources": [
        "vcam_sender/vcam_sender.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "libraries": []
          }
        ]
      ]
    },
    {
      "target_name": "virtual_cam",
      "type": "shared_library",
      "sources": [
        "vcam/vcam_filter.cpp"
      ],
      "include_dirs": [],
      "defines": [
        "_USRDLL"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "libraries": [
              "-lstrmiids.lib",
              "-lole32.lib",
              "-loleaut32.lib",
              "-luuid.lib",
              "-ladvapi32.lib"
            ],
            "msvs_settings": {
              "VCLinkerTool": {
                "ModuleDefinitionFile": "../vcam/vcam_filter.def"
              }
            }
          }
        ]
      ]
    },
    {
      "target_name": "spotify_capture",
      "sources": [
        "spotify_capture/spotify_capture.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "UNICODE",
        "_UNICODE",
        "NOMINMAX"
      ],
      "conditions": [
        [
          "OS=='win'",
          {
            "libraries": [
              "-lmmdevapi.lib",
              "-lole32.lib"
            ],
            "msvs_settings": {
              "VCCLCompilerTool": {
                "ExceptionHandling": 1
              }
            }
          }
        ]
      ]
    }
  ]
}
