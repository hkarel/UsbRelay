import qbs
import qbs.File

Product {
    name: "UsbRelay"
    targetName: "usbrelay"

    type: "staticlibrary"

    Depends { name: "cpp" }
    Depends { name: "SharedLib" }
    Depends { name: "Qt"; submodules: ["core"] }

    cpp.cxxFlags: [
        "-ggdb3",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
    ]
    cpp.includePaths: ["."]
    cpp.cxxLanguageVersion: "c++17"

    // Декларация для подавления Qt warning-ов
    cpp.systemIncludePaths: Qt.core.cpp.includePaths

    files: [
        "usb_relay.cpp",
        "usb_relay.h",
    ]
    Export {
        Depends { name: "cpp" }
        cpp.includePaths: [".."]
    }
}
