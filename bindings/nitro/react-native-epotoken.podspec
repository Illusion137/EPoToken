require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name             = "react-native-epotoken"
  s.version          = package["version"]
  s.summary          = package["description"]
  s.homepage         = "https://github.com/Illusion137/EPoToken"
  s.license          = "MIT"
  s.authors          = { "Illusion137" => "primalllusion@gmail.com" }
  s.platforms        = { :ios => "13.4" }
  s.source           = { :git => "https://github.com/Illusion137/EPoToken.git", :tag => "#{s.version}" }

  s.source_files = [
    "cpp/*.{hpp,cpp}",
    "cpp/core/include/**/*.{h,hpp}",
    "cpp/core/src/**/*.{h,hpp,cpp}",
  ]

  s.pod_target_xcconfig = {
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
    "GCC_PREPROCESSOR_DEFINITIONS" => "NLOHMANN_JSON_USE_IMPLICIT_CONVERSIONS=1",
    "HEADER_SEARCH_PATHS" =>
      "\"${PODS_TARGET_SRCROOT}/cpp\" "                    \
      "\"${PODS_TARGET_SRCROOT}/cpp/core/include\" "       \
      "\"${PODS_TARGET_SRCROOT}/cpp/core/src\""
  }

  install_modules_dependencies(s)
end
