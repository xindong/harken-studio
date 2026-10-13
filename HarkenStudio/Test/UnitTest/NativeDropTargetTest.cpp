#include <QtGlobal>

#ifdef Q_OS_WIN

#include "Platform/NativeDropTarget_Win.h"

#include <gtest/gtest.h>

#include <QImage>
#include <QScopeGuard>
#include <wrl/client.h>
#include <vector>

namespace {

class DropData final : public IDataObject
{
public:
    explicit DropData(bool internal)
    {
        formats_.push_back({CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        if (internal)
            formats_.push_back({static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
                L"application/x-harken-source-paths")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *result = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* requested) override
    {
        for (const auto& format : formats_)
            if (format.cfFormat == requested->cfFormat) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override
    {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats_.size()), formats_.data(), formats);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG references_ = 1;
    std::vector<FORMATETC> formats_;
};

class QtDropTarget final : public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDropTarget) return E_NOINTERFACE;
        *result = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        entered = data;
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        ++moves;
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { ++leaves; return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        dropped = data;
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }

    IDataObject* entered = nullptr;
    IDataObject* dropped = nullptr;
    int moves = 0;
    int leaves = 0;

private:
    ULONG references_ = 1;
};

class DropImageHelper final : public IDropTargetHelper
{
public:
    explicit DropImageHelper(IDropTargetHelper* helper) : helper_(helper) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
    {
        *result = nullptr;
        if (id != IID_IUnknown && id != IID_IDropTargetHelper) return E_NOINTERFACE;
        *result = static_cast<IDropTargetHelper*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { return --references_; }
    HRESULT STDMETHODCALLTYPE DragEnter(HWND window, IDataObject* data, POINT* point, DWORD effect) override
    {
        hideRequested = false;
        return helper_->DragEnter(window, data, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(POINT* point, DWORD effect) override
    {
        hideRequested = false;
        return helper_->DragOver(point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        hideRequested = false;
        return helper_->DragLeave();
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, POINT* point, DWORD effect) override
    {
        hideRequested = false;
        return helper_->Drop(data, point, effect);
    }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override
    {
        const HRESULT result = helper_->Show(show);
        hideRequested = SUCCEEDED(result) && !show;
        return result;
    }

    bool hideRequested = false;

private:
    IDropTargetHelper* helper_;
    ULONG references_ = 1;
};

}

TEST(NativeDropTarget, InternalSourcesKeepTheirMimeAndMoveActionThroughWindows)
{
    DropData data(true);
    QtDropTarget qt;
    int externalMoves = 0;
    int externalDrops = 0;
    int externalLeaves = 0;
    VNX::WrappingDropTarget target(&qt,
        [&](const VNX::NativeDropInfo&) { ++externalDrops; },
        [&](const QStringList&, bool) { ++externalMoves; return true; },
        [&]() { ++externalLeaves; });
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    EXPECT_EQ(target.DragEnter(&data, MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(qt.entered, &data);
    EXPECT_EQ(effect, DROPEFFECT_MOVE);
    EXPECT_EQ(target.DragOver(MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(qt.moves, 1);
    EXPECT_EQ(effect, DROPEFFECT_MOVE);
    EXPECT_EQ(target.Drop(&data, 0, POINTL{}, &effect), S_OK);
    EXPECT_EQ(qt.dropped, &data);
    EXPECT_EQ(effect, DROPEFFECT_MOVE);
    EXPECT_EQ(target.DragEnter(&data, MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(target.DragLeave(), S_OK);
    EXPECT_EQ(qt.leaves, 1);
    EXPECT_EQ(externalMoves, 0);
    EXPECT_EQ(externalDrops, 0);
    EXPECT_EQ(externalLeaves, 0);
}

TEST(NativeDropTarget, ExternalFilesStillUseTheExternalDispatcher)
{
    DropData data(false);
    QtDropTarget qt;
    int moves = 0;
    int leaves = 0;
    bool accepted = true;
    VNX::WrappingDropTarget target(&qt, {},
        [&](const QStringList&, bool layeredFeedback) {
            EXPECT_FALSE(layeredFeedback);
            ++moves;
            return accepted;
        },
        [&]() { ++leaves; });
    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    EXPECT_EQ(target.DragEnter(&data, MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(qt.entered, nullptr);
    EXPECT_EQ(effect, DROPEFFECT_COPY);
    EXPECT_EQ(target.DragOver(MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(moves, 2);
    EXPECT_EQ(qt.moves, 0);
    accepted = false;
    EXPECT_EQ(target.DragOver(MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(effect, DROPEFFECT_NONE);
    accepted = true;
    EXPECT_EQ(target.DragOver(MK_LBUTTON, POINTL{}, &effect), S_OK);
    EXPECT_EQ(effect, DROPEFFECT_COPY);
    EXPECT_EQ(target.DragLeave(), S_OK);
    EXPECT_EQ(leaves, 1);
    EXPECT_EQ(qt.leaves, 0);
}

TEST(NativeDropTarget, ShellPreviewStaysHiddenAndRestoresFeedbackAfterLeaveAndDrop)
{
    ASSERT_TRUE(SUCCEEDED(OleInitialize(nullptr)));
    const auto apartment = qScopeGuard([]() { OleUninitialize(); });
    Microsoft::WRL::ComPtr<IDataObject> data;
    ASSERT_TRUE(SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&data))));
    const wchar_t path[] = L"C:\\Audio\\Sample.wav\0";
    STGMEDIUM files = {};
    files.tymed = TYMED_HGLOBAL;
    files.hGlobal = GlobalAlloc(GHND, sizeof(DROPFILES) + sizeof(path));
    ASSERT_NE(files.hGlobal, nullptr);
    auto* dropFiles = static_cast<DROPFILES*>(GlobalLock(files.hGlobal));
    ASSERT_NE(dropFiles, nullptr);
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;
    memcpy(reinterpret_cast<char*>(dropFiles) + sizeof(DROPFILES), path, sizeof(path));
    GlobalUnlock(files.hGlobal);
    FORMATETC fileFormat = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    ASSERT_TRUE(SUCCEEDED(data->SetData(&fileFormat, &files, TRUE)));
    Microsoft::WRL::ComPtr<IDragSourceHelper2> source;
    ASSERT_TRUE(SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&source))));
    ASSERT_TRUE(SUCCEEDED(source->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT)));
    QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    SHDRAGIMAGE nativeImage = {{20, 20}, {0, 0}, image.toHBITMAP(), CLR_NONE};
    ASSERT_TRUE(SUCCEEDED(source->InitializeFromBitmap(&nativeImage, data.Get())));
    Microsoft::WRL::ComPtr<IDropTargetHelper> target;
    ASSERT_TRUE(SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&target))));
    DropImageHelper feedback(target.Get());
    HWND window = CreateWindowExW(0, L"STATIC", L"NativeDragFeedbackTest", 0,
                                 -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    const auto destroyWindow = qScopeGuard([&]() { DestroyWindow(window); });
    POINTL position = {-32000, -32000};
    const auto layered = [&]() {
        FORMATETC format = {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"IsShowingLayered")),
                           nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&format, &medium))) return false;
        const auto* value = static_cast<BOOL*>(GlobalLock(medium.hGlobal));
        const bool result = value && *value;
        GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return result;
    };
    bool accepted = true;
    int drops = 0;
    int leaves = 0;
    int feedbacks = 0;
    QtDropTarget qt;
    {
        VNX::WrappingDropTarget wrapped(&qt,
            [&](const VNX::NativeDropInfo& info) {
                EXPECT_EQ(info.paths, QStringList{QStringLiteral("C:\\Audio\\Sample.wav")});
                ++drops;
            },
            [&](const QStringList& paths, bool layeredFeedback) {
                EXPECT_EQ(paths, QStringList{QStringLiteral("C:\\Audio\\Sample.wav")});
                EXPECT_TRUE(layeredFeedback);
                EXPECT_TRUE(feedback.hideRequested);
                ++feedbacks;
                return accepted;
            },
            [&]() { ++leaves; }, window, &feedback);
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        ASSERT_EQ(wrapped.DragEnter(data.Get(), MK_LBUTTON, position, &effect), S_OK);
        EXPECT_TRUE(layered());
        EXPECT_EQ(effect, DROPEFFECT_COPY);
        EXPECT_TRUE(feedback.hideRequested);
        accepted = false;
        ASSERT_EQ(wrapped.DragOver(MK_LBUTTON, position, &effect), S_OK);
        EXPECT_TRUE(layered());
        EXPECT_TRUE(feedback.hideRequested);
        EXPECT_EQ(effect, DROPEFFECT_NONE);
        ASSERT_EQ(wrapped.DragLeave(), S_OK);
        EXPECT_FALSE(layered());
        EXPECT_FALSE(feedback.hideRequested);
        EXPECT_EQ(leaves, 1);
        accepted = true;
        ASSERT_EQ(wrapped.DragEnter(data.Get(), MK_LBUTTON, position, &effect), S_OK);
        ASSERT_EQ(wrapped.Drop(data.Get(), 0, position, &effect), S_OK);
        EXPECT_FALSE(layered());
        EXPECT_FALSE(feedback.hideRequested);
        EXPECT_EQ(effect, DROPEFFECT_COPY);
        EXPECT_EQ(drops, 1);
        EXPECT_EQ(leaves, 2);
        ASSERT_EQ(wrapped.DragEnter(data.Get(), MK_LBUTTON, position, &effect), S_OK);
        EXPECT_TRUE(layered());
        EXPECT_EQ(feedbacks, 4);
    }
    EXPECT_FALSE(layered());
    EXPECT_FALSE(feedback.hideRequested);
    EXPECT_EQ(qt.entered, nullptr);
    EXPECT_EQ(qt.dropped, nullptr);
}

#endif
