/*
 * vbmcp_mem.hpp - C++ RAII 内存助手（MCP 服务器模块统一使用）
 *
 * 项目已从 C 迁移到 C++17：凡是"申请 → 用 → 释放"的资源都改用 RAII，
 * 避免异常路径 / 提前 return 时泄漏。
 *
 * 说明：底层仍用 CRT malloc/free（与 VB Decompiler 进程模型一致），
 * 这里只提供类型安全的智能指针封装，不改变分配器语义。
 */
#ifndef VBMCP_MEM_HPP
#define VBMCP_MEM_HPP

#include <cstdlib>
#include <memory>
#include <string>

namespace vbmcp {

/* free 删除器：配合 std::unique_ptr 管理 vbmcp_alloc / malloc 的裸指针。 */
struct FreeDeleter {
    void operator()(void* p) const noexcept { if (p) ::free(p); }
};

/* 按 vbmcp_free 语义释放的 unique_ptr（等价 FreeDeleter，语义更清晰）。 */
using OwnedString = std::unique_ptr<char, FreeDeleter>;

/* RAII 套接字：析构时自动 closesocket，避免异常路径漏关。 */
class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET s) : sock_(s) {}
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& o) noexcept : sock_(o.release()) {}
    SocketHandle& operator=(SocketHandle&& o) noexcept
    {
        if (this != &o) reset(o.release());
        return *this;
    }
    ~SocketHandle() { reset(); }

    SOCKET get() const noexcept { return sock_; }
    SOCKET release() noexcept { SOCKET s = sock_; sock_ = INVALID_SOCKET; return s; }
    void reset(SOCKET s = INVALID_SOCKET) noexcept
    {
        if (sock_ != INVALID_SOCKET && sock_ != s) ::closesocket(sock_);
        sock_ = s;
    }
    bool valid() const noexcept { return sock_ != INVALID_SOCKET; }
    explicit operator bool() const noexcept { return valid(); }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

/* 作用域守卫：析构时执行 fn（用于"离开作用域即恢复标志位"这类逻辑）。 */

} // namespace vbmcp

#endif /* VBMCP_MEM_HPP */