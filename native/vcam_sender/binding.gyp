{
  "targets": [
    {
      "target_name": "vcam_sender",
      "sources": ["vcam_sender.cpp"],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
      "conditions": [
        ["OS=='win'", {
          "libraries": []
        }]
      ]
    }
  ]
}
