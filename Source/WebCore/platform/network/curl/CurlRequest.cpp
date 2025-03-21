/*
 * Copyright (C) 2018 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "CurlRequest.h"

#if USE(CURL)

#include "CertificateInfo.h"
#include "CurlRequestClient.h"
#include "CurlRequestScheduler.h"
#include "MIMETypeRegistry.h"
#include "NetworkLoadMetrics.h"
#include "ResourceError.h"
#include "SharedBuffer.h"
#include <wtf/Language.h>
#include <wtf/MainThread.h>

#if USE(ULTRALIGHT)
#include <Ultralight/private/tracy/Tracy.hpp>
#endif

namespace WebCore {

CurlRequest::CurlRequest(const ResourceRequest&request, CurlRequestClient* client, ShouldSuspend shouldSuspend, EnableMultipart enableMultipart, CaptureNetworkLoadMetrics captureExtraMetrics, MessageQueue<Function<void()>>* messageQueue)
    : m_client(client)
    , m_messageQueue(messageQueue)
    , m_request(request.isolatedCopy())
    , m_shouldSuspend(shouldSuspend == ShouldSuspend::Yes)
    , m_enableMultipart(enableMultipart == EnableMultipart::Yes)
    , m_formDataStream(m_request.httpBody())
    , m_captureExtraMetrics(captureExtraMetrics == CaptureNetworkLoadMetrics::Extended)
{
    ASSERT(isMainThread());
}

void CurlRequest::invalidateClient()
{
    ASSERT(isMainThread());

    //auto locker = holdLock(m_clientMutex);
    m_client = nullptr;
    m_messageQueue = nullptr;
}

void CurlRequest::setAuthenticationScheme(ProtectionSpaceAuthenticationScheme scheme)
{
    switch (scheme) {
    case ProtectionSpaceAuthenticationSchemeHTTPBasic:
        m_authType = CURLAUTH_BASIC;
        break;

    case ProtectionSpaceAuthenticationSchemeHTTPDigest:
        m_authType = CURLAUTH_DIGEST;
        break;

    case ProtectionSpaceAuthenticationSchemeNTLM:
        m_authType = CURLAUTH_NTLM;
        break;

    case ProtectionSpaceAuthenticationSchemeNegotiate:
        m_authType = CURLAUTH_NEGOTIATE;
        break;

    default:
        m_authType = CURLAUTH_ANY;
        break;
    }
}

void CurlRequest::setUserPass(const String& user, const String& password)
{
    ASSERT(isMainThread());

    m_user = user.isolatedCopy();
    m_password = password.isolatedCopy();
}

void CurlRequest::start()
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    // The pausing of transfer does not work with protocols, like file://.
    // Therefore, PAUSE can not be done in didReceiveData().
    // It means that the same logic as http:// can not be used.
    // In the file scheme, invokeDidReceiveResponse() is done first. 
    // Then StartWithJobManager is called with completeDidReceiveResponse and start transfer with libcurl.

    // http : didReceiveHeader => didReceiveData[PAUSE] => invokeDidReceiveResponse => (MainThread)curlDidReceiveResponse => completeDidReceiveResponse[RESUME] => didReceiveData
    // file : invokeDidReceiveResponseForFile => (MainThread)curlDidReceiveResponse => completeDidReceiveResponse => didReceiveData

    ASSERT(isMainThread());

    auto url = m_request.url().isolatedCopy();

    if (std::isnan(m_requestStartTime))
        m_requestStartTime = MonotonicTime::now().isolatedCopy();

    if (url.isLocalFile())
        invokeDidReceiveResponseForFile(url);
    else
        startWithJobManager();
}

void CurlRequest::startWithJobManager()
{
    ASSERT(isMainThread());

    CurlContext::singleton().scheduler().add(this);
}

void CurlRequest::cancel()
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    ASSERT(isMainThread());

    {
        auto locker = holdLock(m_statusMutex);
        if (m_cancelled)
            return;

        m_cancelled = true;
    }

    auto& scheduler = CurlContext::singleton().scheduler();

    if (needToInvokeDidCancelTransfer()) {
        runOnWorkerThreadIfRequired([this, protectedThis = makeRef(*this)]() {
            didCancelTransfer();
        });
    } else
        scheduler.cancel(this);

    invalidateClient();
}

bool CurlRequest::isCancelled()
{
    auto locker = holdLock(m_statusMutex);
    return m_cancelled;
}

bool CurlRequest::isCompletedOrCancelled()
{
    auto locker = holdLock(m_statusMutex);
    return m_completed || m_cancelled;
}

void CurlRequest::suspend()
{
    ASSERT(isMainThread());

    setRequestPaused(true);
}

void CurlRequest::resume()
{
    ASSERT(isMainThread());

    setRequestPaused(false);
}

/* `this` is protected inside this method. */
void CurlRequest::callClient(Function<void(CurlRequest&, CurlRequestClient&)>&& task)
{
    runOnMainThread([this, protectedThis = makeRef(*this), task = WTFMove(task)]() mutable {
        if (m_client)
            task(*this, makeRef(*m_client));
    });
}

void CurlRequest::runOnMainThread(Function<void()>&& task)
{
    if (m_messageQueue)
        m_messageQueue->append(std::make_unique<Function<void()>>(WTFMove(task)));
    else if (isMainThread())
        task();
    else
        callOnMainThread(WTFMove(task));
}

void CurlRequest::runOnWorkerThreadIfRequired(Function<void()>&& task)
{
    if (isMainThread())
        CurlContext::singleton().scheduler().callOnWorkerThread(WTFMove(task));
    else
        task();
}

CURL* CurlRequest::setupTransfer()
{
    auto httpHeaderFields = m_request.httpHeaderFields();
    appendAcceptLanguageHeader(httpHeaderFields);

    m_curlHandle = std::make_unique<CurlHandle>();

    m_curlHandle->setUrl(m_request.url());

    m_curlHandle->appendRequestHeaders(httpHeaderFields);

    const auto& method = m_request.httpMethod();
    if (method == "GET")
        m_curlHandle->enableHttpGetRequest();
    else if (method == "POST")
        setupPOST(m_request);
    else if (method == "PUT")
        setupPUT(m_request);
    else if (method == "HEAD")
        m_curlHandle->enableHttpHeadRequest();
    else {
        m_curlHandle->setHttpCustomRequest(method);
        setupPUT(m_request);
    }

    if (!m_user.isEmpty() || !m_password.isEmpty()) {
        m_curlHandle->setHttpAuthUserPass(m_user, m_password, m_authType);
    }

    if (m_shouldDisableServerTrustEvaluation)
        m_curlHandle->disableServerTrustEvaluation();

    m_curlHandle->setHeaderCallbackFunction(didReceiveHeaderCallback, this);
    m_curlHandle->setWriteCallbackFunction(didReceiveDataCallback, this);

    if (m_captureExtraMetrics)
        m_curlHandle->setDebugCallbackFunction(didReceiveDebugInfoCallback, this);

    m_curlHandle->setTimeout(timeoutInterval());

    if (m_shouldSuspend)
        setRequestPaused(true);

    m_performStartTime = MonotonicTime::now();

    return m_curlHandle->handle();
}

Seconds CurlRequest::timeoutInterval() const
{
    // Request specific timeout interval.
    if (m_request.timeoutInterval())
        return Seconds { m_request.timeoutInterval() };

    // Default timeout interval set by application.
    if (m_request.defaultTimeoutInterval())
        return Seconds { m_request.defaultTimeoutInterval() };

    // Use platform default timeout interval.
    return CurlContext::singleton().defaultTimeoutInterval();
}

// This is called to obtain HTTP POST or PUT data.
// Iterate through FormData elements and upload files.
// Carefully respect the given buffer size and fill the rest of the data at the next calls.

size_t CurlRequest::willSendData(char* buffer, size_t blockSize, size_t numberOfBlocks)
{
    if (isCompletedOrCancelled())
        return CURL_READFUNC_ABORT;

    if (!blockSize || !numberOfBlocks)
        return CURL_READFUNC_ABORT;

    // Check for overflow.
    if (blockSize > (std::numeric_limits<size_t>::max() / numberOfBlocks))
        return CURL_READFUNC_ABORT;

    size_t bufferSize = blockSize * numberOfBlocks;
    auto sendBytes = m_formDataStream.read(buffer, bufferSize);
    if (!sendBytes) {
        // Something went wrong so error the job.
        return CURL_READFUNC_ABORT;
    }

    callClient([totalReadSize = m_formDataStream.totalReadSize(), totalSize = m_formDataStream.totalSize()](CurlRequest& request, CurlRequestClient& client) {
        client.curlDidSendData(request, totalReadSize, totalSize);
    });

    return *sendBytes;
}

// This is being called for each HTTP header in the response. This includes '\r\n'
// for the last line of the header.

size_t CurlRequest::didReceiveHeader(String&& header)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    static const auto emptyLineCRLF = "\r\n";
    static const auto emptyLineLF = "\n";

    if (isCompletedOrCancelled())
        return 0;

    // libcurl sends all headers that libcurl received to application.
    // So, in digest authentication, a block of response headers are received twice consecutively from libcurl.
    // For example, when authentication succeeds, the first block is "401 Authorization", and the second block is "200 OK".
    // Also, "100 Continue" and "200 Connection Established" do the same behavior.
    // In this process, deletes the first block to send a correct headers to WebCore.
    if (m_didReceiveResponse) {
        m_didReceiveResponse = false;
        m_response = CurlResponse { };
        m_multipartHandle = nullptr;
    }

    auto receiveBytes = static_cast<size_t>(header.length());

    // The HTTP standard requires to use \r\n but for compatibility it recommends to accept also \n.
    if ((header != emptyLineCRLF) && (header != emptyLineLF)) {
        m_response.headers.append(WTFMove(header));
        return receiveBytes;
    }

    long statusCode = 0;
    if (auto code = m_curlHandle->getResponseCode())
        statusCode = *code;

    long httpConnectCode = 0;
    if (auto code = m_curlHandle->getHttpConnectCode())
        httpConnectCode = *code;

    m_didReceiveResponse = true;

    m_response.url = m_request.url();
    m_response.statusCode = statusCode;
    m_response.httpConnectCode = httpConnectCode;

    if (auto length = m_curlHandle->getContentLength())
        m_response.expectedContentLength = *length;

    if (auto proxyUrl = m_curlHandle->getProxyUrl())
        m_response.proxyUrl = URL(URL(), *proxyUrl);

    if (auto auth = m_curlHandle->getHttpAuthAvail())
        m_response.availableHttpAuth = *auth;

    if (auto auth = m_curlHandle->getProxyAuthAvail())
        m_response.availableProxyAuth = *auth;

    if (auto version = m_curlHandle->getHttpVersion())
        m_response.httpVersion = *version;

    if (m_response.availableProxyAuth)
        CurlContext::singleton().setProxyAuthMethod(m_response.availableProxyAuth);

    if (auto info = m_curlHandle->certificateInfo())
        m_response.certificateInfo = WTFMove(*info);

    m_response.networkLoadMetrics = networkLoadMetrics();

    if (m_enableMultipart)
        m_multipartHandle = CurlMultipartHandle::createIfNeeded(*this, m_response);

    // Response will send at didReceiveData() or didCompleteTransfer()
    // to receive continueDidRceiveResponse() for asynchronously.

    return receiveBytes;
}

// called with data after all headers have been processed via headerCallback

size_t CurlRequest::didReceiveData(Ref<SharedBuffer>&& buffer)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    ASSERT(!isMainThread());

    if (isCompletedOrCancelled())
        return 0;

    if (needToInvokeDidReceiveResponse()) {
        // Pause until completeDidReceiveResponse() is called.
        setCallbackPaused(true);
        invokeDidReceiveResponse(m_response, Action::ReceiveData);
        // Because libcurl pauses the handle after returning this CURL_WRITEFUNC_PAUSE,
        // we need to update its state here.
        updateHandlePauseState(true);
        return CURL_WRITEFUNC_PAUSE;
    }

    auto receiveBytes = buffer->size();
    m_totalReceivedSize += receiveBytes;

    writeDataToDownloadFileIfEnabled(buffer);

    if (receiveBytes) {
        if (m_multipartHandle) {

            m_multipartHandle->didReceiveData(buffer);
        } else {
            m_receiveBufferQueue.enqueue(std::move(buffer));
            if (!m_pendingConsumeRequest) {
                m_pendingConsumeRequest.store(true);
                callClient([&](CurlRequest& request, CurlRequestClient& client) mutable {
                    m_pendingConsumeRequest.store(false);
                    client.curlConsumeReceiveQueue(request, m_receiveBufferQueue);
                });
            }
        }
    }

    return receiveBytes;
}

void CurlRequest::didReceiveHeaderFromMultipart(const Vector<String>& headers)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    if (isCompletedOrCancelled())
        return;

    CurlResponse response = m_response.isolatedCopy();
    response.expectedContentLength = 0;
    response.headers.clear();

    for (auto header : headers)
        response.headers.append(header);

    invokeDidReceiveResponse(response, Action::None);
}

void CurlRequest::didReceiveDataFromMultipart(Ref<SharedBuffer>&& buffer)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    ASSERT(!isMainThread());

    if (isCompletedOrCancelled())
        return;

    auto receiveBytes = buffer->size();

    if (receiveBytes) {
        m_receiveBufferQueue.enqueue(std::move(buffer));
        if (!m_pendingConsumeRequest) {
            m_pendingConsumeRequest.store(true);
            callClient([=](CurlRequest& request, CurlRequestClient& client) mutable {
                m_pendingConsumeRequest.store(false);
                client.curlConsumeReceiveQueue(request, m_receiveBufferQueue);
            });
        }
    }
}

void CurlRequest::didCompleteTransfer(CURLcode result)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    if (isCancelled()) {
        didCancelTransfer();
        return;
    }

    if (needToInvokeDidReceiveResponse()) {
        // Processing of didReceiveResponse() has not been completed. (For example, HEAD method)
        // When completeDidReceiveResponse() is called, didCompleteTransfer() will be called again.

        m_finishedResultCode = result;
        invokeDidReceiveResponse(m_response, Action::FinishTransfer);
        return;
    }

    if (result == CURLE_OK) {
        if (m_multipartHandle)
            m_multipartHandle->didComplete();

        auto metrics = networkLoadMetrics();

        finalizeTransfer();
        callClient([requestStartTime = m_requestStartTime.isolatedCopy(), networkLoadMetrics = WTFMove(metrics)](CurlRequest& request, CurlRequestClient& client) mutable {
            networkLoadMetrics.responseEnd = MonotonicTime::now() - requestStartTime;
            networkLoadMetrics.markComplete();

            client.curlDidComplete(request, WTFMove(networkLoadMetrics));
        });
    } else {
        auto type = (result == CURLE_OPERATION_TIMEDOUT && timeoutInterval()) ? ResourceError::Type::Timeout : ResourceError::Type::General;
        auto resourceError = ResourceError::httpError(result, m_request.url(), type);
        if (auto sslErrors = m_curlHandle->sslErrors())
            resourceError.setSslErrors(sslErrors);

        CertificateInfo certificateInfo;
        if (auto info = m_curlHandle->certificateInfo())
            certificateInfo = WTFMove(*info);

        finalizeTransfer();
        callClient([error = WTFMove(resourceError), certificateInfo = WTFMove(certificateInfo)](CurlRequest& request, CurlRequestClient& client) mutable {
            client.curlDidFailWithError(request, WTFMove(error), WTFMove(certificateInfo));
        });
    }

    {
        auto locker = holdLock(m_statusMutex);
        m_completed = true;
    }
}

void CurlRequest::didCancelTransfer()
{
    finalizeTransfer();
    cleanupDownloadFile();
}

void CurlRequest::finalizeTransfer()
{
    closeDownloadFile();
    m_formDataStream.clean();
    m_multipartHandle = nullptr;
    m_curlHandle = nullptr;
}

int CurlRequest::didReceiveDebugInfo(curl_infotype type, char* data, size_t size)
{
    if (!data)
        return 0;

    if (type == CURLINFO_HEADER_OUT) {
        String requestHeader(data, size);
        auto headerFields = requestHeader.split("\r\n");
        // Remove the request line
        if (headerFields.size())
            headerFields.remove(0);

        for (auto& header : headerFields) {
            auto pos = header.find(":");
            if (pos != notFound) {
                auto key = header.left(pos).stripWhiteSpace();
                auto value = header.substring(pos + 1).stripWhiteSpace();
                m_requestHeaders.add(key, value);
            }
        }
    }

    return 0;
}

void CurlRequest::appendAcceptLanguageHeader(HTTPHeaderMap& header)
{
    for (const auto& language : userPreferredLanguages())
        header.add(HTTPHeaderName::AcceptLanguage, language);
}

void CurlRequest::setupPUT(ResourceRequest& request)
{
    m_curlHandle->enableHttpPutRequest();

    // Disable the Expect: 100 continue header
    m_curlHandle->removeRequestHeader("Expect");

    auto elementSize = m_formDataStream.elementSize();
    if (!elementSize)
        return;

    setupSendData(true);
}

void CurlRequest::setupPOST(ResourceRequest& request)
{
    m_curlHandle->enableHttpPostRequest();

    auto elementSize = m_formDataStream.elementSize();

    if (!m_request.hasHTTPHeader(HTTPHeaderName::ContentType) && !elementSize)
        m_curlHandle->removeRequestHeader("Content-Type"_s);

    if (!elementSize)
        return;

    // Do not stream for simple POST data
    if (elementSize == 1) {
        const auto* postData = m_formDataStream.getPostData();
        if (postData && postData->size())
            m_curlHandle->setPostFields(postData->data(), postData->size());
    } else
        setupSendData(false);
}

void CurlRequest::setupSendData(bool forPutMethod)
{
    // curl guesses that we want chunked encoding as long as we specify the header
    if (m_formDataStream.shouldUseChunkTransfer())
        m_curlHandle->appendRequestHeader("Transfer-Encoding: chunked");
    else {
        if (forPutMethod)
            m_curlHandle->setInFileSizeLarge(static_cast<curl_off_t>(m_formDataStream.totalSize()));
        else
            m_curlHandle->setPostFieldLarge(static_cast<curl_off_t>(m_formDataStream.totalSize()));
    }

    m_curlHandle->setReadCallbackFunction(willSendDataCallback, this);
}

void CurlRequest::invokeDidReceiveResponseForFile(URL& url)
{
    // Since the code in didReceiveHeader() will not have run for local files
    // the code to set the URL and fire didReceiveResponse is never run,
    // which means the ResourceLoader's response does not contain the URL.
    // Run the code here for local files to resolve the issue.

    ASSERT(isMainThread());
    ASSERT(url.isLocalFile());

    m_response.url = url;
    m_response.statusCode = 200;

    // Determine the MIME type based on the path.
    m_response.headers.append(String("Content-Type: " + MIMETypeRegistry::getMIMETypeForPath(m_response.url.path())));

    // DidReceiveResponse must not be called immediately
    runOnWorkerThreadIfRequired([this, protectedThis = makeRef(*this)]() {
        invokeDidReceiveResponse(m_response, Action::StartTransfer);
    });
}

void CurlRequest::invokeDidReceiveResponse(const CurlResponse& response, Action behaviorAfterInvoke)
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    ASSERT(!m_didNotifyResponse || m_multipartHandle);

    m_didNotifyResponse = true;
    m_actionAfterInvoke = behaviorAfterInvoke;

    // FIXME: Replace this isolatedCopy with WTFMove.
    callClient([response = response.isolatedCopy()](CurlRequest& request, CurlRequestClient& client) mutable {
        client.curlDidReceiveResponse(request, WTFMove(response));
    });
}

void CurlRequest::completeDidReceiveResponse()
{
#if USE(ULTRALIGHT)
    ProfiledZone;
#endif
    ASSERT(isMainThread());
    ASSERT(m_didNotifyResponse);
    ASSERT(!m_didReturnFromNotify || m_multipartHandle);

    if (isCompletedOrCancelled())
        return;

    m_didReturnFromNotify = true;

    if (m_actionAfterInvoke == Action::ReceiveData) {
        // Resume transfer
        setCallbackPaused(false);
    } else if (m_actionAfterInvoke == Action::StartTransfer) {
        // Start transfer for file scheme
        startWithJobManager();
    } else if (m_actionAfterInvoke == Action::FinishTransfer) {
        runOnWorkerThreadIfRequired([this, protectedThis = makeRef(*this), finishedResultCode = m_finishedResultCode]() {
            didCompleteTransfer(finishedResultCode);
        });
    }
}

void CurlRequest::setRequestPaused(bool paused)
{
    {
        LockHolder lock(m_pauseStateMutex);

        auto savedState = shouldBePaused();
        m_shouldSuspend = m_isPausedOfRequest = paused;
        if (shouldBePaused() == savedState)
            return;
    }

    pausedStatusChanged();
}

void CurlRequest::setCallbackPaused(bool paused)
{
    {
        LockHolder lock(m_pauseStateMutex);

        auto savedState = shouldBePaused();
        m_isPausedOfCallback = paused;

        // If pause is requested, it is called within didReceiveData() which means
        // actual change happens inside libcurl. No need to update manually here.
        if (shouldBePaused() == savedState || paused)
            return;
    }

    pausedStatusChanged();
}

void CurlRequest::invokeCancel()
{
    // There's no need to extract this method. This is a workaround for MSVC's bug
    // which happens when using lambda inside other lambda. The compiler loses context
    // of `this` which prevent makeRef.
    runOnMainThread([this, protectedThis = makeRef(*this)]() {
        cancel();
    });
}

void CurlRequest::pausedStatusChanged()
{
    if (isCompletedOrCancelled())
        return;

    runOnWorkerThreadIfRequired([this, protectedThis = makeRef(*this)]() {
        if (isCompletedOrCancelled() || !m_curlHandle)
            return;

        bool needCancel { false };
        {
            LockHolder lock(m_pauseStateMutex);
            bool paused = shouldBePaused();

            if (isHandlePaused() == paused)
                return;

            auto error = m_curlHandle->pause(paused ? CURLPAUSE_ALL : CURLPAUSE_CONT);
            if (error == CURLE_OK)
                updateHandlePauseState(paused);

            needCancel = (error != CURLE_OK && !paused);
        }

        if (needCancel)
            invokeCancel();
    });
}

void CurlRequest::updateHandlePauseState(bool paused)
{
    ASSERT(!isMainThread());
    m_isHandlePaused = paused;
}

bool CurlRequest::isHandlePaused() const
{
    ASSERT(!isMainThread());
    return m_isHandlePaused;
}

NetworkLoadMetrics CurlRequest::networkLoadMetrics()
{
    ASSERT(m_curlHandle);

    auto domainLookupStart = m_performStartTime - m_requestStartTime;
    auto networkLoadMetrics = m_curlHandle->getNetworkLoadMetrics(domainLookupStart);
    if (!networkLoadMetrics)
        return NetworkLoadMetrics();

    if (m_captureExtraMetrics) {
        m_curlHandle->addExtraNetworkLoadMetrics(*networkLoadMetrics);
        networkLoadMetrics->requestHeaders = m_requestHeaders;
        networkLoadMetrics->responseBodyDecodedSize = m_totalReceivedSize;
    }

    return WTFMove(*networkLoadMetrics);
}

void CurlRequest::enableDownloadToFile()
{
    LockHolder locker(m_downloadMutex);
    m_isEnabledDownloadToFile = true;
}

const String& CurlRequest::getDownloadedFilePath()
{
    LockHolder locker(m_downloadMutex);
    return m_downloadFilePath;
}

void CurlRequest::writeDataToDownloadFileIfEnabled(const SharedBuffer& buffer)
{
    {
        LockHolder locker(m_downloadMutex);

        if (!m_isEnabledDownloadToFile)
            return;

        if (m_downloadFilePath.isEmpty())
            m_downloadFilePath = FileSystem::openTemporaryFile("download", m_downloadFileHandle);
    }

    if (m_downloadFileHandle != FileSystem::invalidPlatformFileHandle)
        FileSystem::writeToFile(m_downloadFileHandle, buffer.data(), buffer.size());
}

void CurlRequest::closeDownloadFile()
{
    LockHolder locker(m_downloadMutex);

    if (m_downloadFileHandle == FileSystem::invalidPlatformFileHandle)
        return;

    FileSystem::closeFile(m_downloadFileHandle);
    m_downloadFileHandle = FileSystem::invalidPlatformFileHandle;
}

void CurlRequest::cleanupDownloadFile()
{
    LockHolder locker(m_downloadMutex);

    if (!m_downloadFilePath.isEmpty()) {
        FileSystem::deleteFile(m_downloadFilePath);
        m_downloadFilePath = String();
    }
}

size_t CurlRequest::willSendDataCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->willSendData(ptr, blockSize, numberOfBlocks);
}

size_t CurlRequest::didReceiveHeaderCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveHeader(String(ptr, blockSize * numberOfBlocks));
}

size_t CurlRequest::didReceiveDataCallback(char* ptr, size_t blockSize, size_t numberOfBlocks, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveData(SharedBuffer::create(ptr, blockSize * numberOfBlocks));
}

int CurlRequest::didReceiveDebugInfoCallback(CURL*, curl_infotype type, char* data, size_t size, void* userData)
{
    return static_cast<CurlRequest*>(userData)->didReceiveDebugInfo(type, data, size);
}

}

#endif
