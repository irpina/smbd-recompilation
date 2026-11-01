#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace smbd {
using InstallProgress=std::function<void(unsigned,std::string_view)>;
std::string verify_install_disc(const std::filesystem::path&,const InstallProgress& progress={});
std::string install_game_files(const std::filesystem::path& iso,const std::filesystem::path& directory,bool copy_disc,
                               const InstallProgress& progress={},const std::atomic_bool* cancel=nullptr);
}
