/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim:set ts=4 sw=4 sts=4 cindent et: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsHTTPCompressConv.h"
#include "nsMemory.h"
#include "plstr.h"
#include "nsCOMPtr.h"
#include "nsError.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"
#include "nsComponentManagerUtils.h"
#include "nsThreadUtils.h"
#include "mozilla/Preferences.h"

// nsISupports implementation
NS_IMPL_ISUPPORTS(nsHTTPCompressConv,
                  nsIStreamConverter,
                  nsIStreamListener,
                  nsIRequestObserver)

// nsFTPDirListingConv methods
nsHTTPCompressConv::nsHTTPCompressConv()
    : mListener(nullptr)
    , mMode(HTTP_COMPRESS_IDENTITY)
    , mOutBuffer(nullptr)
    , mInpBuffer(nullptr)
    , mOutBufferLen(0)
    , mInpBufferLen(0)
    , mCheckHeaderDone(false)
    , mStreamEnded(false)
    , mStreamInitialized(false)
    , mLen(0)
    , hMode(0)
    , mSkipCount(0)
    , mFlags(0)
{
    if (NS_IsMainThread()) {
        mFailUncleanStops =
            (Preferences::GetBool("network.http.enforce-framing.soft", false) ||
             Preferences::GetBool("network.http.enforce-framing.http", false));
    } else {
        mFailUncleanStops = false;
    }
}

nsHTTPCompressConv::~nsHTTPCompressConv()
{
    NS_IF_RELEASE(mListener);

    if (mInpBuffer)
        nsMemory::Free(mInpBuffer);

    if (mOutBuffer)
        nsMemory::Free(mOutBuffer);

    // For some reason we are not getting Z_STREAM_END.  But this was also seen
    //    for mozilla bug 198133.  Need to handle this case.
    if (mStreamInitialized && !mStreamEnded)
        inflateEnd (&d_stream);
}

NS_IMETHODIMP
nsHTTPCompressConv::AsyncConvertData(const char *aFromType, 
                                     const char *aToType, 
                                     nsIStreamListener *aListener, 
                                     nsISupports *aCtxt)
{
    if (!PL_strncasecmp(aFromType, HTTP_COMPRESS_TYPE, sizeof(HTTP_COMPRESS_TYPE)-1) ||
        !PL_strncasecmp(aFromType, HTTP_X_COMPRESS_TYPE, sizeof(HTTP_X_COMPRESS_TYPE)-1))
        mMode = HTTP_COMPRESS_COMPRESS;

    else if (!PL_strncasecmp(aFromType, HTTP_GZIP_TYPE, sizeof(HTTP_GZIP_TYPE)-1) ||
             !PL_strncasecmp(aFromType, HTTP_X_GZIP_TYPE, sizeof(HTTP_X_GZIP_TYPE)-1))
        mMode = HTTP_COMPRESS_GZIP;

    else if (!PL_strncasecmp(aFromType, HTTP_DEFLATE_TYPE, sizeof(HTTP_DEFLATE_TYPE)-1))
        mMode = HTTP_COMPRESS_DEFLATE;

    // hook ourself up with the receiving listener. 
    mListener = aListener;
    NS_ADDREF(mListener);

    mAsyncConvContext = aCtxt;
    return NS_OK; 
} 

NS_IMETHODIMP
nsHTTPCompressConv::OnStartRequest(nsIRequest* request, nsISupports *aContext)
{
    return mListener->OnStartRequest(request, aContext);
} 

NS_IMETHODIMP
nsHTTPCompressConv::OnStopRequest(nsIRequest* request, nsISupports *aContext, 
                                  nsresult aStatus)
{
    if (!mStreamEnded && NS_SUCCEEDED(aStatus) && mFailUncleanStops) {
        // This is not a clean end of stream, the transfer is incomplete.
        aStatus = NS_ERROR_NET_PARTIAL_TRANSFER;
    }

    return mListener->OnStopRequest(request, aContext, aStatus);
} 

NS_IMETHODIMP
nsHTTPCompressConv::OnDataAvailable(nsIRequest* request, 
                                    nsISupports *aContext, 
                                    nsIInputStream *iStr, 
                                    uint64_t aSourceOffset, 
                                    uint32_t aCount)
{
    nsresult rv = NS_ERROR_INVALID_CONTENT_ENCODING;
    uint32_t streamLen = aCount;

    if (streamLen == 0)
    {
        NS_ERROR("count of zero passed to OnDataAvailable");
        return NS_ERROR_UNEXPECTED;
    }

    if (mStreamEnded)
    {
        // Hmm... this may just indicate that the data stream is done and that
        // what's left is either metadata or padding of some sort.... throwing
        // it out is probably the safe thing to do.
        uint32_t n;
        return iStr->ReadSegments(NS_DiscardSegment, nullptr, streamLen, &n);
    }

    switch (mMode)
    {
        case HTTP_COMPRESS_GZIP:
            streamLen = check_header(iStr, streamLen, &rv);

            if (rv != NS_OK)
                return rv;

            if (streamLen == 0)
                return NS_OK;

            // FALLTHROUGH

        case HTTP_COMPRESS_DEFLATE:

            if (mInpBuffer != nullptr && streamLen > mInpBufferLen)
            {
                mInpBuffer = (unsigned char *) moz_realloc(mInpBuffer, mInpBufferLen = streamLen);
               
                if (mOutBufferLen < streamLen * 2)
                    mOutBuffer = (unsigned char *) moz_realloc(mOutBuffer, mOutBufferLen = streamLen * 3);

                if (mInpBuffer == nullptr || mOutBuffer == nullptr)
                    return NS_ERROR_OUT_OF_MEMORY;
            }

            if (mInpBuffer == nullptr)
                mInpBuffer = (unsigned char *) moz_malloc(mInpBufferLen = streamLen);

            if (mOutBuffer == nullptr)
                mOutBuffer = (unsigned char *) moz_malloc(mOutBufferLen = streamLen * 3);

            if (mInpBuffer == nullptr || mOutBuffer == nullptr)
                return NS_ERROR_OUT_OF_MEMORY;

            uint32_t unused;
            iStr->Read((char *)mInpBuffer, streamLen, &unused);

            if (mMode == HTTP_COMPRESS_DEFLATE)
            {
                if (!mStreamInitialized)
                {
                    memset(&d_stream, 0, sizeof (d_stream));
                
                    if (inflateInit(&d_stream) != Z_OK)
                        return NS_ERROR_FAILURE;

                    mStreamInitialized = true;
                }
                d_stream.next_in = mInpBuffer;
                d_stream.avail_in = (uInt)streamLen;

                mDummyStreamInitialised = false;
                for (;;)
                {
                    d_stream.next_out = mOutBuffer;
                    d_stream.avail_out = (uInt)mOutBufferLen;
 
                    int code = inflate(&d_stream, Z_NO_FLUSH);
                    unsigned bytesWritten = (uInt)mOutBufferLen - d_stream.avail_out;

                    if (code == Z_STREAM_END)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                        
                        inflateEnd(&d_stream);
                        mStreamEnded = true;
                        break;
                    }
                    else if (code == Z_OK)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                    }
                    else if (code == Z_BUF_ERROR)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                        break;
                    }
                    else if (code == Z_DATA_ERROR)
                    {
                        // some servers (notably Apache with mod_deflate) don't generate zlib headers
                        // insert a dummy header and try again
                        static char dummy_head[2] =
                        {
                            0x8 + 0x7 * 0x10,
                            (((0x8 + 0x7 * 0x10) * 0x100 + 30) / 31 * 31) & 0xFF,
                        };
                        inflateReset(&d_stream);
                        d_stream.next_in = (Bytef*) dummy_head;
                        d_stream.avail_in = sizeof(dummy_head);

                        code = inflate(&d_stream, Z_NO_FLUSH);
                        if (code != Z_OK)
                            return NS_ERROR_FAILURE;

                        // stop an endless loop caused by non-deflate data being labelled as deflate
                        if (mDummyStreamInitialised) {
                            NS_WARNING("endless loop detected"
                                       " - invalid deflate");
                            return NS_ERROR_INVALID_CONTENT_ENCODING;
                        }
                        mDummyStreamInitialised = true;
                        // reset stream pointers to our original data
                        d_stream.next_in = mInpBuffer;
                        d_stream.avail_in = (uInt)streamLen;
                    }    
                    else
                        return NS_ERROR_INVALID_CONTENT_ENCODING;
                } /* for */
            }
            else
            {
                if (!mStreamInitialized)
                {
                    memset(&d_stream, 0, sizeof (d_stream));
                
                    if (inflateInit2(&d_stream, -MAX_WBITS) != Z_OK)
                        return NS_ERROR_FAILURE;

                    mStreamInitialized = true;
                }

                d_stream.next_in  = mInpBuffer;
                d_stream.avail_in = (uInt)streamLen;

                for (;;)
                {
                    d_stream.next_out  = mOutBuffer;
                    d_stream.avail_out = (uInt)mOutBufferLen;
 
                    int code = inflate (&d_stream, Z_NO_FLUSH);
                    unsigned bytesWritten = (uInt)mOutBufferLen - d_stream.avail_out;

                    if (code == Z_STREAM_END)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                        
                        inflateEnd(&d_stream);
                        mStreamEnded = true;
                        break;
                    }
                    else if (code == Z_OK)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                    }
                    else if (code == Z_BUF_ERROR)
                    {
                        if (bytesWritten)
                        {
                            rv = do_OnDataAvailable(request, aContext, aSourceOffset, (char *)mOutBuffer, bytesWritten);
                            if (NS_FAILED (rv))
                                return rv;
                        }
                        break;
                    }
                    else
                        return NS_ERROR_INVALID_CONTENT_ENCODING;
                } /* for */
            } /* gzip */
            break;

        default: 
            rv = mListener->OnDataAvailable(request, aContext, iStr, aSourceOffset, aCount);
            if (NS_FAILED (rv))
                return rv;
    } /* switch */

	return NS_OK;
} /* OnDataAvailable */


// XXX/ruslan: need to implement this too

NS_IMETHODIMP
nsHTTPCompressConv::Convert(nsIInputStream *aFromStream, 
                            const char *aFromType, 
                            const char *aToType, 
                            nsISupports *aCtxt, 
                            nsIInputStream **_retval)
{ 
    return NS_ERROR_NOT_IMPLEMENTED;
} 

nsresult
nsHTTPCompressConv::do_OnDataAvailable(nsIRequest* request,
                                       nsISupports *context, uint64_t offset,
                                       const char *buffer, uint32_t count)
{
    if (!mStream) {
        mStream = do_CreateInstance(NS_STRINGINPUTSTREAM_CONTRACTID);
        NS_ENSURE_STATE(mStream);
    }

    mStream->ShareData(buffer, count);

    nsresult rv = mListener->OnDataAvailable(request, context, mStream,
                                             offset, count);

    // Make sure the stream no longer references |buffer| in case our listener
    // is crazy enough to try to read from |mStream| after ODA.
    mStream->ShareData("", 0);

    return rv;
}

#define ASCII_FLAG   0x01 /* bit 0 set: file probably ascii text */
#define HEAD_CRC     0x02 /* bit 1 set: header CRC present */
#define EXTRA_FIELD  0x04 /* bit 2 set: extra field present */
#define ORIG_NAME    0x08 /* bit 3 set: original file name present */
#define COMMENT      0x10 /* bit 4 set: file comment present */
#define RESERVED     0xE0 /* bits 5..7: reserved */

static unsigned gz_magic[2] = {0x1f, 0x8b}; /* gzip magic header */

uint32_t
nsHTTPCompressConv::check_header(nsIInputStream *iStr, uint32_t streamLen, nsresult *rs)
{
    enum  { GZIP_INIT = 0, GZIP_OS, GZIP_EXTRA0, GZIP_EXTRA1, GZIP_EXTRA2, GZIP_ORIG, GZIP_COMMENT, GZIP_CRC };
    char c;

    *rs = NS_OK;

    if (mCheckHeaderDone)
        return streamLen;

    while (streamLen)
    {
        switch (hMode)
        {
            case GZIP_INIT:
                uint32_t unused;
                iStr->Read(&c, 1, &unused);
                streamLen--;
                
                if (mSkipCount == 0 && ((unsigned)c & 0377) != gz_magic[0])
                {
                    *rs = NS_ERROR_INVALID_CONTENT_ENCODING;
                    return 0;
                }

                if (mSkipCount == 1 && ((unsigned)c & 0377) != gz_magic[1])
                {
                    *rs = NS_ERROR_INVALID_CONTENT_ENCODING;
                    return 0;
                }

                if (mSkipCount == 2 && ((unsigned)c & 0377) != Z_DEFLATED)
                {
                    *rs = NS_ERROR_INVALID_CONTENT_ENCODING;
                    return 0;
                }

                mSkipCount++;
                if (mSkipCount == 4)
                {
                    mFlags = (unsigned) c & 0377;
                    if (mFlags & RESERVED)
                    {
                        *rs = NS_ERROR_INVALID_CONTENT_ENCODING;
                        return 0;
                    }
                    hMode = GZIP_OS;
                    mSkipCount = 0;
                }
                break;

            case GZIP_OS:
                iStr->Read(&c, 1, &unused);
                streamLen--;
                mSkipCount++;

                if (mSkipCount == 6)
                    hMode = GZIP_EXTRA0;
                break;
        
            case GZIP_EXTRA0:
                if (mFlags & EXTRA_FIELD)
                {
                    iStr->Read(&c, 1, &unused);
                    streamLen--;
                    mLen = (uInt) c & 0377;
                    hMode = GZIP_EXTRA1;
                }
                else
                    hMode = GZIP_ORIG;
                break;

            case GZIP_EXTRA1:
                iStr->Read(&c, 1, &unused);
                streamLen--;
                mLen = ((uInt) c & 0377) << 8;
                mSkipCount = 0;
                hMode = GZIP_EXTRA2;
                break;

            case GZIP_EXTRA2:
                if (mSkipCount == mLen)
                    hMode = GZIP_ORIG;
                else
                {
                    iStr->Read(&c, 1, &unused);
                    streamLen--;
                    mSkipCount++;
                }
                break;

            case GZIP_ORIG:
                if (mFlags & ORIG_NAME)
                {
                    iStr->Read(&c, 1, &unused);
                    streamLen--;
                    if (c == 0)
                        hMode = GZIP_COMMENT;
                }
                else
                    hMode = GZIP_COMMENT;
                break;

            case GZIP_COMMENT:
                if (mFlags & COMMENT)
                {
                    iStr->Read(&c, 1, &unused);
                    streamLen--;
                    if (c == 0)
                    {
                        hMode = GZIP_CRC;
                        mSkipCount = 0;
                    }
                }
                else
                {
                    hMode = GZIP_CRC;
                    mSkipCount = 0;
                }
                break;

            case GZIP_CRC:
                if (mFlags & HEAD_CRC)
                {
                    iStr->Read(&c, 1, &unused);
                    streamLen--;
                    mSkipCount++;
                    if (mSkipCount == 2)
                    {
                        mCheckHeaderDone = true;
                        return streamLen;
                    }
                }
                else
                {
                    mCheckHeaderDone = true;
                    return streamLen;
                }
            break;
        }
    }
    return streamLen;
}

nsresult
NS_NewHTTPCompressConv(nsHTTPCompressConv **aHTTPCompressConv)
{
    NS_PRECONDITION(aHTTPCompressConv != nullptr, "null ptr");

    if (!aHTTPCompressConv)
        return NS_ERROR_NULL_POINTER;

    *aHTTPCompressConv = new nsHTTPCompressConv();

    if (!*aHTTPCompressConv)
        return NS_ERROR_OUT_OF_MEMORY;

    NS_ADDREF(*aHTTPCompressConv);
    return NS_OK;
}
