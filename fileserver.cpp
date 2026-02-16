#include "fileserver.h"

#include <fstream>
#include <stdexcept>

namespace Http {

FileServer::FileServer(Server& server, const std::string& baseDir)
    : server_(server), baseDir_(baseDir) {}

std::string FileServer::contentTypeForExtension(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";

    std::string ext = filename.substr(dot);
    if (ext == ".html") return "text/html";
    if (ext == ".js")   return "text/javascript";
    if (ext == ".css")  return "text/css";
    if (ext == ".png")  return "image/png";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".json") return "application/json";
    if (ext == ".ico")  return "image/x-icon";
    return "application/octet-stream";
}

void FileServer::add(const std::string& filename) {
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        throw std::runtime_error("Invalid filename: " + filename);
    }

    std::string fullPath = baseDir_ + "/" + filename;
    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("File not found: " + fullPath);
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    std::string contentType = contentTypeForExtension(filename);

    server_.get("/" + filename, [content, contentType](Context&) {
        return Ok(content, contentType);
    });

    if (filename == "index.html") {
        server_.get("/", [content, contentType](Context&) {
            return Ok(content, contentType);
        });
    }
}

void FileServer::add(std::initializer_list<std::string> filenames) {
    for (const auto& filename : filenames) {
        add(filename);
    }
}

} // namespace Http
