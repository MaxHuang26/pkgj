#include "vitahttp.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include <fmt/format.h>

#include <boost/scope_exit.hpp>

#define PKGI_USER_AGENT "libhttp/3.65 (PS Vita)"

struct pkgi_http
{
    int used;

    int tmpl;
    int conn;
    int req;
};

namespace
{
static pkgi_http g_http[4];
}

VitaHttp::~VitaHttp()
{
    if (_http)
    {
        LOG("http close");
        sceHttpDeleteRequest(_http->req);
        sceHttpDeleteConnection(_http->conn);
        sceHttpDeleteTemplate(_http->tmpl);
        _http->used = 0;
    }
}

void VitaHttp::start(const std::string& url, uint64_t offset)
{
    if (_http)
        throw HttpError("HTTP 連接已啓動");

    LOG("http get");

    pkgi_http* http = NULL;
    for (size_t i = 0; i < 4; i++)
    {
        if (g_http[i].used == 0)
        {
            http = g_http + i;
            break;
        }
    }

    if (!http)
        throw HttpError("内部錯誤: 同時發起的連接過多");

    int tmpl = -1;
    int conn = -1;
    int req = -1;

    LOGF("starting http GET request for {}", url);

    if ((tmpl = sceHttpCreateTemplate(
                 PKGI_USER_AGENT, SCE_HTTP_VERSION_1_1, SCE_TRUE)) < 0)
        throw HttpError(fmt::format(
                "創建模板失敗: {:#08x}",
                static_cast<uint32_t>(tmpl)));
    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (tmpl > 0)
            sceHttpDeleteTemplate(tmpl);
    };
    // sceHttpSetRecvTimeOut(tmpl, 10 * 1000 * 1000);

    if ((conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), SCE_FALSE)) <
        0)
        throw HttpError(fmt::format(
                "創建與鏈接的連接失敗: {:#08x}",
                static_cast<uint32_t>(conn)));
    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (conn > 0)
            sceHttpDeleteConnection(conn);
    };

    if ((req = sceHttpCreateRequestWithURL(
                 conn, SCE_HTTP_METHOD_GET, url.c_str(), 0)) < 0)
        throw HttpError(fmt::format(
                "創建鏈接的請求失敗: {:#08x}",
                static_cast<uint32_t>(req)));
    BOOST_SCOPE_EXIT_ALL(&)
    {
        if (req > 0)
            sceHttpDeleteRequest(req);
    };

    int err;

    if (offset != 0)
    {
        char range[64];
        pkgi_snprintf(range, sizeof(range), "bytes=%llu-", offset);
        if ((err = sceHttpAddRequestHeader(
                     req, "Range", range, SCE_HTTP_HEADER_ADD)) < 0)
            throw HttpError(fmt::format(
                    "添加請求文件頭失敗: {:#08x}",
                    static_cast<uint32_t>(err)));
    }

    if ((err = sceHttpSendRequest(req, NULL, 0)) < 0)
        throw formatEx<HttpError>(
                "發送請求失敗: {:#08x}\n{}",
                static_cast<uint32_t>(err),
                static_cast<uint32_t>(err) == 0x80431075
                        ? "PSVita 無法支持 HTTPS 链接"
                          ".\n請更換 HTTP 鏈接."
                        : "");

    http->used = 1;
    http->tmpl = tmpl;
    http->conn = conn;
    http->req = req;
    tmpl = conn = req = -1;

    _http = http;
}

int64_t VitaHttp::read(uint8_t* buffer, uint64_t size)
{
    int read = sceHttpReadData(_http->req, buffer, size);
    if (read < 0)
        throw HttpError(fmt::format(
                "下載錯誤 {:#08x}",
                static_cast<uint32_t>(static_cast<int32_t>(read))));
    return read;
}

int64_t VitaHttp::get_length()
{
    int res;
    int status;
    if ((res = sceHttpGetStatusCode(_http->req, &status)) < 0)
        throw HttpError(fmt::format(
                "獲取狀態代碼失敗: {:#08x}",
                static_cast<uint32_t>(res)));

    LOGF("http status code = {}", status);

    if (status != 200 && status != 206)
        throw HttpError(fmt::format("錯誤的 HTTP 狀態: {}", status));

    uint64_t content_length;
    res = sceHttpGetResponseContentLength(_http->req, &content_length);
    if (res < 0)
        throw HttpError(fmt::format(
                "獲取響應内容長度失敗: {:#08x}",
                static_cast<uint32_t>(res)));
    if (res == (int)SCE_HTTP_ERROR_NO_CONTENT_LENGTH ||
        res == (int)SCE_HTTP_ERROR_CHUNK_ENC)
    {
        LOG("http response has no content length (or chunked "
            "encoding)");
        return 0;
    }

    LOGF("http response length = {}", content_length);
    return content_length;
}

VitaHttp::operator bool() const
{
    return _http;
}
