#pragma once

#include "http.h"

#include <string>
#include <initializer_list>

namespace Http {

class FileServer {
public:
    FileServer(Server& server, const std::string& baseDir);
    void add(const std::string& filename);
    void add(std::initializer_list<std::string> filenames);

private:
    Server& server_;
    std::string baseDir_;

    static std::string contentTypeForExtension(const std::string& filename);
};

} // namespace Http
