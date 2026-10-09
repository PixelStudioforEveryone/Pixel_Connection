#include "hos_controller.h"
#include "pxc/protocol.h"
#include "pxc/session_auth.h"
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace pxc::hos {
std::shared_ptr<rtc::DataChannel> HosController::fileChannel() {
    std::scoped_lock lk(core_mtx_, sess_mtx_);
    return session_authenticated_ && session_ ? session_->channel(pxc::kChFile) : nullptr;
}
bool HosController::fileChannelReady() {
    const auto ch = fileChannel();
    return ch && ch->isOpen();
}
bool HosController::fileCommand(const std::string& text) {
    const auto ch = fileChannel();
    if (!ch || !ch->isOpen()) return false;
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    ch->send(j.dump()); return true;
}
void HosController::fileProgress(const std::string& state, bool done) {
    // All callers hold file_mtx_; events are asynchronously marshalled to ArkTS.
    emit("fileProgress", {{"id",file_id_},{"name",file_name_},{"total",file_total_},
        {"transferred",file_transferred_},{"state",state},{"done",done}});
}
void HosController::finishFile(const std::string& state, bool completed) {
    std::lock_guard lk(file_mtx_);
    if (file_id_.empty()) return;
    if (file_fd_ >= 0) { close(file_fd_); file_fd_ = -1; }
    fileProgress(state, true);
    file_id_.clear(); file_name_.clear(); file_started_ = false;
    file_total_ = file_transferred_ = 0;
}
std::string HosController::uploadFile(int fd, const std::string& name, const std::string& directory) {
    std::lock_guard lk(file_mtx_);
    if (!file_id_.empty() || !fileChannelReady() || name.empty() || name=="." || name==".." ||
        name.find_first_of("/\\")!=std::string::npos || directory.empty()) return {};
    struct stat info{};
    if (fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size < 0) return {};
    file_fd_ = dup(fd);
    if (file_fd_ < 0) return {};
    file_id_ = pxc::make_session_nonce(); file_name_ = name;
    file_total_ = info.st_size; file_transferred_ = 0; file_upload_ = true; file_started_ = false;
    const auto id = file_id_;
    if (!fileCommand(nlohmann::json({{"op","upload_request"},{"id",id},{"name",name},
            {"size",file_total_},{"path",directory}}).dump())) { finishFile("文件通道未就绪"); return {}; }
    fileProgress("等待远端接受");
    watchFile(id);
    return id;
}
std::string HosController::downloadFile(int fd, const std::string& path, const std::string& name) {
    std::lock_guard lk(file_mtx_);
    if (!file_id_.empty() || !fileChannelReady() || path.empty()) return {};
    file_fd_ = dup(fd); if (file_fd_ < 0) return {};
    file_id_ = pxc::make_session_nonce(); file_name_ = name;
    file_total_ = file_transferred_ = 0; file_upload_ = false; file_started_ = false;
    const auto id = file_id_;
    if (!fileCommand(nlohmann::json({{"op","download_request"},{"id",id},{"path",path}}).dump())) {
        finishFile("文件通道未就绪"); return {};
    }
    fileProgress("等待下载"); watchFile(id); return id;
}
void HosController::watchFile(const std::string& id) {
    file_activity_ = std::chrono::steady_clock::now();
    runAsync([this,id] {
        while (!destroyed_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard lk(file_mtx_);
            if (file_id_ != id) return;
            if (std::chrono::steady_clock::now() - file_activity_ > std::chrono::minutes(5)) {
                fileCommand(nlohmann::json({{"op","transfer_cancel"},{"id",id}}).dump());
                finishFile("传输超时"); return;
            }
        }
    });
}
void HosController::cancelFileTransfer(const std::string& id) {
    std::lock_guard lk(file_mtx_);
    if (id.empty() || id!=file_id_) return;
    fileCommand(nlohmann::json({{"op","transfer_cancel"},{"id",id},{"message","已取消"}}).dump());
    finishFile("已取消");
}
void HosController::handleFileMessage(const nlohmann::json& j) {
    if (!sessionAuthenticated() || !j.is_object()) return;
    std::lock_guard lk(file_mtx_);
    const auto op = j.value("op",std::string());
    const auto id = j.value("id",std::string());
    emit("fileEvent",j);
    if (op=="upload_offer") {
        // Controller-only mobile receives files only after its own download request.
        fileCommand(nlohmann::json({{"op","upload_reject"},{"id",id}}).dump()); return;
    }
    if (file_id_.empty() || id!=file_id_) return;
    file_activity_ = std::chrono::steady_clock::now();
    if (op=="upload_accept" && file_upload_ && !file_started_) {
        file_started_=true; const auto ch=fileChannel();
        if (ch) runAsync([this,id,ch] { pumpFile(id,ch); });
    } else if (op=="download_start" && !file_upload_ && !file_started_) {
        const auto size=j.value("size",int64_t(-1));
        if (size<0 || ftruncate(file_fd_,0) || lseek(file_fd_,0,SEEK_SET)<0) {
            cancelFileTransfer(id); return;
        }
        file_total_=size; file_started_=true; fileProgress("下载中");
    } else if (op=="transfer_end" && !file_upload_ && file_started_) {
        if (file_transferred_!=file_total_ || fsync(file_fd_)!=0) {
            fileCommand(nlohmann::json({{"op","transfer_error"},{"id",id},{"message","文件大小不匹配或写入失败"}}).dump());
            finishFile("文件大小不匹配或写入失败");
        } else {
            fileCommand(nlohmann::json({{"op","transfer_complete"},{"id",id}}).dump());
            finishFile("已完成",true);
        }
    } else if (op=="transfer_complete" && file_upload_ && file_transferred_==file_total_) {
        finishFile("已完成",true);
    } else if (op=="transfer_error" || op=="transfer_cancel" || op=="upload_reject") {
        finishFile(j.value("message",std::string("远端拒绝传输")));
    }
}
void HosController::handleFileChunk(const rtc::binary& bytes) {
    if (!sessionAuthenticated()) return;
    std::lock_guard lk(file_mtx_);
    if (file_id_.empty() || file_upload_ || !file_started_ || file_fd_<0) return;
    if (bytes.size()>size_t(file_total_-file_transferred_)) {
        cancelFileTransfer(file_id_); return;
    }
    size_t offset=0;
    while (offset<bytes.size()) {
        const auto n=write(file_fd_,bytes.data()+offset,bytes.size()-offset);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { cancelFileTransfer(file_id_); return; }
        offset+=n;
    }
    file_transferred_+=bytes.size(); file_activity_ = std::chrono::steady_clock::now(); fileProgress("下载中");
}
void HosController::pumpFile(const std::string& id, std::shared_ptr<rtc::DataChannel> ch) {
    auto last=std::chrono::steady_clock::now();
    while (!destroyed_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard lk(file_mtx_);
        if (file_id_!=id || file_fd_<0) return;
        if (!ch->isOpen() || ch!=fileChannel()) { finishFile("连接已断开"); return; }
        if (ch->bufferedAmount()>512*1024) continue;
        rtc::binary bytes(48*1024);
        const auto n=pread(file_fd_,bytes.data(),bytes.size(),file_transferred_);
        if (n<0 && errno==EINTR) continue;
        if (n<0 || (n==0 && file_transferred_!=file_total_)) {
            fileCommand(nlohmann::json({{"op","transfer_error"},{"id",id},{"message","读取文件失败"}}).dump());
            finishFile("读取文件失败"); return;
        }
        if (n==0) { ch->send(nlohmann::json({{"op","transfer_end"},{"id",id}}).dump()); return; }
        bytes.resize(n); ch->send(bytes); file_transferred_+=n;
        file_activity_ = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now()-last>std::chrono::milliseconds(100)) {
            fileProgress("上传中"); last=std::chrono::steady_clock::now();
        }
    }
}
} // namespace pxc::hos
