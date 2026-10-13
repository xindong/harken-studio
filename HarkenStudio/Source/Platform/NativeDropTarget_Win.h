
#pragma once

#ifdef Q_OS_WIN

#include <Windows.h>
#include <oleidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <QStringList>
#include <QPoint>
#include <functional>
#include <atomic>

namespace VNX {

struct NativeDropInfo {
    QStringList paths;
    QPoint position;
};

using NativeDropCallback = std::function<void(const NativeDropInfo&)>;
using NativeDragFeedback = std::function<bool(const QStringList& paths, bool layeredFeedback)>;
using NativeDragLeaveCallback = std::function<void()>;

class WrappingDropTarget : public IDropTarget
{
public:
    WrappingDropTarget(IDropTarget* qtOriginal,
                       NativeDropCallback onDrop,
                       NativeDragFeedback onDragMove,
                       NativeDragLeaveCallback onLeave,
                       HWND window = nullptr,
                       IDropTargetHelper* imageHelper = nullptr)
        : qtTarget_(qtOriginal)
        , onDrop_(std::move(onDrop))
        , onDragMove_(std::move(onDragMove))
        , onDragLeave_(std::move(onLeave))
        , window_(window)
        , imageHelper_(imageHelper)
    {
        if (qtTarget_) qtTarget_->AddRef();
        if (imageHelper_) imageHelper_->AddRef();
    }

    ~WrappingDropTarget()
    {
        if (helperEntered_) imageHelper_->DragLeave();
        if (imageHelper_) imageHelper_->Release();
        if (qtTarget_) qtTarget_->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override  { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto r = --refCount_;
        if (r == 0) delete this;
        return r;
    }


    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState,
                                         POINTL pt, DWORD* pdwEffect) override
    {
        isExternalFileDrag_ = hasHDrop(pDataObj) && !hasInternalMime(pDataObj);

        if (isExternalFileDrag_)
        {
            extractPaths(pDataObj);
            hasPaths_ = !cachedPaths_.isEmpty();
            POINT position = {pt.x, pt.y};
            helperEntered_ = imageHelper_
                && SUCCEEDED(imageHelper_->DragEnter(window_, pDataObj, &position, DROPEFFECT_NONE));
            if (helperEntered_)
                imageHelper_->Show(FALSE);
            layeredFeedback_ = helperEntered_ && isLayeredFeedback(pDataObj);

            const bool accepted = onDragMove_ ? onDragMove_(cachedPaths_, layeredFeedback_) : hasPaths_;
            *pdwEffect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            updateNativeImage(pt, *pdwEffect);

            return S_OK;
        }

        if (qtTarget_)
            return qtTarget_->DragEnter(pDataObj, grfKeyState, pt, pdwEffect);

        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt,
                                        DWORD* pdwEffect) override
    {
        if (isExternalFileDrag_)
        {
            const bool accepted = onDragMove_ ? onDragMove_(cachedPaths_, layeredFeedback_) : hasPaths_;
            *pdwEffect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            updateNativeImage(pt, *pdwEffect);
            return S_OK;
        }

        if (qtTarget_)
            return qtTarget_->DragOver(grfKeyState, pt, pdwEffect);

        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (isExternalFileDrag_)
        {
            isExternalFileDrag_ = false;
            if (helperEntered_)
                imageHelper_->DragLeave();
            helperEntered_ = false;
            layeredFeedback_ = false;
            if (onDragLeave_)
                onDragLeave_();
            return S_OK;
        }

        if (qtTarget_)
            return qtTarget_->DragLeave();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState,
                                    POINTL pt, DWORD* pdwEffect) override
    {
        if (isExternalFileDrag_)
        {
            isExternalFileDrag_ = false;
            extractPaths(pDataObj);
            *pdwEffect = !cachedPaths_.isEmpty() ? DROPEFFECT_COPY : DROPEFFECT_NONE;
            if (helperEntered_) {
                POINT position = {pt.x, pt.y};
                imageHelper_->Drop(pDataObj, &position, *pdwEffect);
            }
            helperEntered_ = false;
            layeredFeedback_ = false;

            if (onDrop_ && !cachedPaths_.isEmpty())
            {
                NativeDropInfo info;
                info.paths = cachedPaths_;
                info.position = QPoint(pt.x, pt.y);
                onDrop_(info);
            }

            if (onDragLeave_)
                onDragLeave_();
            return S_OK;
        }

        if (qtTarget_)
            return qtTarget_->Drop(pDataObj, grfKeyState, pt, pdwEffect);

        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    static bool isLayeredFeedback(IDataObject* data)
    {
        FORMATETC format = {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"IsShowingLayered")),
                           nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium = {};
        if (FAILED(data->GetData(&format, &medium)))
            return false;
        bool layered = false;
        if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(BOOL)) {
            if (const auto* value = static_cast<const BOOL*>(GlobalLock(medium.hGlobal))) {
                layered = *value != FALSE;
                GlobalUnlock(medium.hGlobal);
            }
        }
        ReleaseStgMedium(&medium);
        return layered;
    }

    void updateNativeImage(POINTL point, DWORD effect)
    {
        if (!helperEntered_) return;
        POINT position = {point.x, point.y};
        imageHelper_->DragOver(&position, effect);
        imageHelper_->Show(FALSE);
    }

    static bool hasInternalMime(IDataObject* data)
    {
        IEnumFORMATETC* formats = nullptr;
        if (FAILED(data->EnumFormatEtc(DATADIR_GET, &formats)))
            return false;
        FORMATETC format = {};
        bool internal = false;
        while (formats->Next(1, &format, nullptr) == S_OK) {
            WCHAR name[256] = {};
            const int length = GetClipboardFormatNameW(format.cfFormat, name, 256);
            CoTaskMemFree(format.ptd);
            if (length > 0 && QString::fromWCharArray(name, length)
                    .startsWith(QStringLiteral("application/x-harken-"))) {
                internal = true;
                break;
            }
        }
        formats->Release();
        return internal;
    }

    static bool hasHDrop(IDataObject* pDataObj)
    {
        if (!pDataObj) return false;
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        return pDataObj->QueryGetData(&fmt) == S_OK;
    }

    void extractPaths(IDataObject* pDataObj)
    {
        cachedPaths_.clear();
        if (!pDataObj) return;

        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = {};

        if (SUCCEEDED(pDataObj->GetData(&fmt, &stg)))
        {
            HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
            if (hDrop)
            {
                UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i)
                {
                    WCHAR path[MAX_PATH];
                    if (DragQueryFileW(hDrop, i, path, MAX_PATH))
                        cachedPaths_.append(QString::fromWCharArray(path));
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
        }
    }

    IDropTarget* qtTarget_ = nullptr;
    NativeDropCallback onDrop_;
    NativeDragFeedback onDragMove_;
    NativeDragLeaveCallback onDragLeave_;
    HWND window_ = nullptr;
    IDropTargetHelper* imageHelper_ = nullptr;
    std::atomic<ULONG> refCount_ { 1 };
    QStringList cachedPaths_;
    bool hasPaths_ = false;
    bool isExternalFileDrag_ = false;
    bool helperEntered_ = false;
    bool layeredFeedback_ = false;
};

inline bool installNativeDropTarget(HWND hwnd,
                                     NativeDropCallback onDrop,
                                     NativeDragFeedback onDragMove,
                                     NativeDragLeaveCallback onLeave)
{
    if (!hwnd) return false;

    IDropTarget* qtOriginal = static_cast<IDropTarget*>(
        GetPropW(hwnd, L"OleDropTargetInterface"));
    if (qtOriginal)
        qtOriginal->AddRef();

    RevokeDragDrop(hwnd);

    IDropTargetHelper* imageHelper = nullptr;
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                     IID_IDropTargetHelper, reinterpret_cast<void**>(&imageHelper));
    auto* wrapper = new WrappingDropTarget(qtOriginal, std::move(onDrop),
                                            std::move(onDragMove),
                                            std::move(onLeave), hwnd, imageHelper);
    if (imageHelper)
        imageHelper->Release();
    HRESULT hr = RegisterDragDrop(hwnd, wrapper);

    if (qtOriginal)
        qtOriginal->Release();

    if (FAILED(hr))
    {
        wrapper->Release();
        return false;
    }

    wrapper->Release();
    return true;
}

}

#endif
