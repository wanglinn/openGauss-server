/* -------------------------------------------------------------------------
 *
 * stringinfo.cpp
 *
 * StringInfo provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with palloc().
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/backend/lib/stringinfo.cpp
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "lib/stringinfo.h"
#include "utils/memutils.h"

/*
 * makeStringInfo
 *
 * Create an empty 'StringInfoData' & return a pointer to it.
 */
StringInfo makeStringInfo(void)
{
    StringInfo res;

    res = (StringInfo)palloc(sizeof(StringInfoData));

    initStringInfo(res);

    return res;
}

/* destroy a 'StringInfoData', use this function when call makeStringInfo */
void DestroyStringInfo(StringInfo str)
{
    if (str != NULL) {
        pfree_ext(str->data);
        pfree_ext(str);
    }
}

/*
 * initStringInfo
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
void initStringInfo(StringInfo str)
{
    int size = 1024; /* initial default buffer size */

    str->data = (char*)palloc(size);
    str->maxlen = size;
    resetStringInfo(str);
}

/* free a 'StringInfoData', use this function when call initStringInfo */
void FreeStringInfo(StringInfo str)
{
    if (str != NULL) {
        pfree_ext(str->data);
    }
}

/*
 * resetStringInfo
 *
 * Reset the StringInfo: the data buffer remains valid, but its
 * previous content, if any, is cleared.
 */
void resetStringInfo(StringInfo str)
{
    str->data[0] = '\0';
    str->len = 0;
    str->cursor = 0;
}

/*
 * appendStringInfo
 *
 * Format text data under the control of fmt (an sprintf-style format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
void appendStringInfo(StringInfo str, const char* fmt, ...)
{
    for (;;) {
        va_list args;
        bool success = false;

        /* Try to format the data. */
        va_start(args, fmt);
        success = appendStringInfoVA(str, fmt, args);
        va_end(args);

        if (success) {
            break;
		}

        /* Double the buffer size and try again. */
        enlargeStringInfo(str, str->maxlen);
    }
}

/*
 * appendStringInfoVA
 *
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and append it to whatever is already in str.	If successful
 * return true; if not (because there's not enough space), return false
 * without modifying str.  Typically the caller would enlarge str and retry
 * on false return --- see appendStringInfo for standard usage pattern.
 *
 * XXX This API is ugly, but there seems no alternative given the C spec's
 * restrictions on what can portably be done with va_list arguments: you have
 * to redo va_start before you can rescan the argument list, and we can't do
 * that from here.
 */
bool appendStringInfoVA(StringInfo str, const char* fmt, va_list args)
{
    int avail, nprinted;

    Assert(str != NULL);

    /*
     * If there's hardly any space, don't bother trying, just fail to make the
     * caller enlarge the buffer first.
     */
    avail = str->maxlen - str->len - 1;
    if (avail < 16) {
        return false;
	}

    /*
     * Assert check here is to catch buggy vsnprintf that overruns the
     * specified buffer length.  Solaris 7 in 64-bit mode is an example of a
     * platform with such a bug.
     */
#ifdef USE_ASSERT_CHECKING
    str->data[str->maxlen - 1] = '\0';
#endif

    nprinted = vsnprintf_s(str->data + str->len, (size_t)(str->maxlen - str->len), (size_t)avail, fmt, args);

    Assert(str->data[str->maxlen - 1] == '\0');

    /*
     * Note: some versions of vsnprintf return the number of chars actually
     * stored, but at least one returns -1 on failure. Be conservative about
     * believing whether the print worked.
     */
    if (nprinted >= 0 && nprinted < avail - 1) {
        /* Success.  Note nprinted does not include trailing null. */
        str->len += nprinted;
        return true;
    }

    /* Restore the trailing null so that str is unmodified. */
    str->data[str->len] = '\0';
    return false;
}

/*
 * appendStringInfoString
 *
 * Append a null-terminated string to str.
 * Like appendStringInfo(str, "%s", s) but faster.
 */
void appendStringInfoString(StringInfo str, const char* s)
{
    appendBinaryStringInfo(str, s, strlen(s));
}

/*
 * appendStringInfoChar
 *
 * Append a single byte to str.
 * Like appendStringInfo(str, "%c", ch) but much faster.
 */
void appendStringInfoChar(StringInfo str, char ch)
{
    /* Make more room if needed */
    if (str->len + 1 >= str->maxlen)
        enlargeStringInfo(str, 1);

    /* OK, append the character */
    str->data[str->len] = ch;
    str->len++;
    str->data[str->len] = '\0';
}

/*
 * appendStringInfoSpaces
 *
 * Append the specified number of spaces to a buffer.
 */
void appendStringInfoSpaces(StringInfo str, int count)
{
    if (count > 0) {
        /* Make more room if needed */
        enlargeStringInfo(str, count);

        /* OK, append the spaces */
        while (--count >= 0)
            str->data[str->len++] = ' ';
        str->data[str->len] = '\0';
    }
}

/*
 * appendBinaryStringInfo
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary.
 */
void appendBinaryStringInfo(StringInfo str, const char* data, int datalen)
{
    Assert(str != NULL);

    /* Make more room if needed */
    enlargeStringInfo(str, datalen);

    /* OK, append the data */
    errno_t rc = memcpy_s(str->data + str->len, (size_t)(str->maxlen - str->len), data, (size_t)datalen);
    securec_check(rc, "\0", "\0");
    str->len += datalen;

    /*
     * Keep a trailing null in place, even though it's probably useless for
     * binary data.  (Some callers are dealing with text but call this because
     * their input isn't null-terminated.)
     */
    str->data[str->len] = '\0';
}

/*
 * appendBinaryStringInfoNT
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary. Does not ensure a trailing null-byte exists.
 */
void appendBinaryStringInfoNT(StringInfo str, const char *data, int datalen)
{
    Assert(str != NULL);

    /* Make more room if needed */
    enlargeStringInfo(str, datalen);

    /* OK, append the data */
    errno_t rc = memcpy_s(str->data + str->len, (size_t)(str->maxlen - str->len), data, (size_t)datalen);
    securec_check(rc, "\0", "\0");
    str->len += datalen;
}

/*
 * copyStringInfo
 *
 * Deep copy: Data part is copied too.   Cursor of the destination is
 * initialized to zero.
 */
void copyStringInfo(StringInfo to, StringInfo from)
{
    resetStringInfo(to);
    appendBinaryStringInfo(to, from->data, from->len);
    return;
}

/*
 * enlargeBuffer
 *
 * Make sure there is enough space for 'needed' more bytes
 * ('needed' does not include the terminating null).
 *
 * NB: because we use repalloc() to enlarge the buffer, the string buffer
 * will remain allocated in the same memory context that was current when
 * initStringInfo was called, even if another context is now current.
 * This is the desired and indeed critical behavior!
 */
void enlargeBuffer(int needed,  // needed more bytes
    int len,                    // current used buffer length in bytes
    int* maxlen,               // original/new allocated buffer length
    char** data)             // pointer to original/new buffer
{
    int newlen;

    /*
     * Guard against out-of-range "needed" values.	Without this, we can get
     * an overflow or infinite loop in the following.
     */
    /* should not happen */
    if (needed < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid string enlargement request size: %d", needed)));
    }
    if (((Size)len > MaxAllocSize) || ((Size)needed) >= (MaxAllocSize - (Size)len)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("out of memory"),
                errdetail("Cannot enlarge buffer containing %d bytes by %d more bytes.", len, needed)));
    }

    needed += len + 1; /* total space required now */

    /* Because of the above test, we now have needed <= MaxAllocSize */
    if (needed <= (int)*maxlen) {
        return; /* got enough space already */
    }

    /*
     * We don't want to allocate just a little more space with each append;
     * for efficiency, double the buffer size each time it overflows.
     * Actually, we might need to more than double it if 'needed' is big...
     */
    newlen = 2 * *maxlen;
    while (needed > newlen) {
        newlen = 2 * newlen;
    }

    /*
     * Clamp to MaxAllocSize in case we went past it.  Note we are assuming
     * here that MaxAllocSize <= INT_MAX/2, else the above loop could
     * overflow.  We will still have newlen >= needed.
     */
    if (newlen > (int)MaxAllocSize) {
        newlen = (int)MaxAllocSize;
    }

    *data = (char*)repalloc(*data, newlen);
    *maxlen = newlen;
}

void enlargeBufferSize(int needed,  // needed more bytes
    int len,                    // current used buffer length in bytes
    size_t* maxlen,               // original/new allocated buffer length
    char** data)             // pointer to original/new buffer
{
    int newlen;

    /*
     * Guard against out-of-range "needed" values.	Without this, we can get
     * an overflow or infinite loop in the following.
     */
    /* should not happen */
    if (needed < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid string enlargement request size: %d", needed)));
    }
    if (((Size)len > MaxAllocSize) || ((Size)needed) >= (MaxAllocSize - (Size)len)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("out of memory"),
                errdetail("Cannot enlarge buffer containing %d bytes by %d more bytes.", len, needed)));
    }

    needed += len + 1; /* total space required now */

    /* Because of the above test, we now have needed <= MaxAllocSize */
    if (needed <= (int)*maxlen) {
        return; /* got enough space already */
    }

    /*
     * We don't want to allocate just a little more space with each append;
     * for efficiency, double the buffer size each time it overflows.
     * Actually, we might need to more than double it if 'needed' is big...
     */
    newlen = 2 * *maxlen;
    while (needed > newlen) {
        newlen = 2 * newlen;
    }

    /*
     * Clamp to MaxAllocSize in case we went past it.  Note we are assuming
     * here that MaxAllocSize <= INT_MAX/2, else the above loop could
     * overflow.  We will still have newlen >= needed.
     */
    if (newlen > (int)MaxAllocSize) {
        newlen = (int)MaxAllocSize;
    }

    *data = (char*)repalloc(*data, newlen);
    *maxlen = newlen;
}

// template void enlargeBuffer<int>(int, int, int*, char** data);
// template void enlargeBuffer<size_t>(int, int, size_t*, char** data);

/*
 * enlargeStringInfo
 *
 * Make sure there is enough space for StringInfo
 *
 * External callers usually need not concern themselves with this, since
 * all stringinfo.c routines do it automatically.  However, if a caller
 * knows that a StringInfo will eventually become X bytes large, it
 * can save some palloc overhead by enlarging the buffer before starting
 * to store data in it.
 */
void enlargeStringInfo(StringInfo str, int needed)
{
    enlargeBuffer(needed, str->len, &str->maxlen, &str->data);
}

/*
 * The following function is used to request the use of more than 1GB of memory
 */

/*
 * initStringInfoHuge
 *
 * Initialize a StringInfoDataHuge struct (with previously undefined contents)
 * to describe an empty string.
 */
void initStringInfoHuge(StringInfoHuge str)
{
    int64 size = 1024; /* initial default buffer size */

    str->data = (char*)palloc(size);
    str->maxlen = size;
    resetStringInfoHuge(str);
}

/* free a 'StringInfoDataHuge', use this function when call initStringInfo */
void FreeStringInfoHuge(StringInfoHuge str)
{
    if (str != NULL) {
        pfree_ext(str->data);
    }
}

/*
 * resetStringInfoHuge
 *
 * Reset the StringInfoHuge: the data buffer remains valid, but its
 * previous content, if any, is cleared.
 */
void resetStringInfoHuge(StringInfoHuge str)
{
    str->data[0] = '\0';
    str->len = 0;
    str->cursor = 0;
}

/*
 * enlargeBufferHuge
 *
 * Make sure there is enough space for 'needed' more bytes
 * ('needed' does not include the terminating null).
 * No upper memory limit.
 */
void enlargeBufferHuge(int64 needed,  // needed more bytes
    int64 len,                    // current used buffer length in bytes
    int64* maxlen,               // original/new allocated buffer length
    char** data)             // pointer to original/new buffer
{
    int64 newlen;

    /*
     * Guard against out-of-range "needed" values.	Without this, we can get
     * an overflow or infinite loop in the following.
     */
    /* should not happen */
    if (needed < 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid string enlargement request size: %ld", needed)));
    }

    if (((Size)len > MaxAllocHugeSize) || ((Size)needed) >= (MaxAllocHugeSize - (Size)len)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("out of memory"),
                errdetail("Cannot enlarge buffer containing %ld bytes by %ld more bytes.", len, needed)));
    }

    needed += len + 1; /* total space required now */

    /* Because of the above test, we now have needed <= MaxAllocSize */
    if (needed <= (int)*maxlen) {
        return; /* got enough space already */
    }

    /*
     * We don't want to allocate just a little more space with each append;
     * for efficiency, double the buffer size each time it overflows.
     * Actually, we might need to more than double it if 'needed' is big...
     */
    newlen = 2 * *maxlen;
    while (needed > newlen) {
        newlen = 2 * newlen;
    }

    /*
     * Clamp to MaxAllocHugeSize in case we went past it.  Note we are assuming
     * here that MaxAllocHugeSize <= SIZE_MAX/2, else the above loop could
     * overflow.  We will still have newlen >= needed.
     */
    if (newlen > (int64)MaxAllocHugeSize) {
        newlen = (int64)MaxAllocHugeSize;
    }

    *data = (char*)repalloc_huge(*data, newlen);
    *maxlen = newlen;
}


/*
 * enlargeStringInfoHuge
 *
 * Make sure there is enough space for StringInfo, No upper memory limit.
 */
void enlargeStringInfoHuge(StringInfoHuge str, int64 needed)
{
    enlargeBufferHuge(needed, str->len, &str->maxlen, &str->data);
}

/*
 * appendStringInfoHugeVA
 *
 * Attempt to format text data under the control of fmt (an sprintf-style
 * format string) and append it to whatever is already in str.	If successful
 * return true; if not (because there's not enough space), return false
 * without modifying str.  Typically the caller would enlarge str and retry
 * on false return --- see appendStringInfo for standard usage pattern.
 *
 * XXX This API is ugly, but there seems no alternative given the C spec's
 * restrictions on what can portably be done with va_list arguments: you have
 * to redo va_start before you can rescan the argument list, and we can't do
 * that from here.
 */
bool appendStringInfoHugeVA(StringInfoHuge str, const char* fmt, va_list args)
{
    int64 avail, nprinted;

    Assert(str != NULL);

    /*
     * If there's hardly any space, don't bother trying, just fail to make the
     * caller enlarge the buffer first.
     */
    avail = str->maxlen - str->len - 1;
    if (avail < 16) {
        return false;
    }

    /*
     * Assert check here is to catch buggy vsnprintf that overruns the
     * specified buffer length.  Solaris 7 in 64-bit mode is an example of a
     * platform with such a bug.
     */
#ifdef USE_ASSERT_CHECKING
    str->data[str->maxlen - 1] = '\0';
#endif

    nprinted = vsnprintf_s(str->data + str->len, (size_t)(str->maxlen - str->len), (size_t)avail, fmt, args);

    Assert(str->data[str->maxlen - 1] == '\0');

    /*
     * Note: some versions of vsnprintf return the number of chars actually
     * stored, but at least one returns -1 on failure. Be conservative about
     * believing whether the print worked.
     */
    if (nprinted >= 0 && nprinted < avail - 1) {
        /* Success.  Note nprinted does not include trailing null. */
        str->len += nprinted;
        return true;
    }

    /* Restore the trailing null so that str is unmodified. */
    str->data[str->len] = '\0';
    return false;
}

/*
 * appendStringInfoHuge
 *
 * Format text data under the control of fmt (an sprintf-style format string)
 * and append it to whatever is already in str.  More space is allocated
 * to str if necessary.  This is sort of like a combination of sprintf and
 * strcat.
 */
void appendStringInfoHuge(StringInfoHuge str, const char* fmt, ...)
{
    for (;;) {
        va_list args;
        bool success = false;

        /* Try to format the data. */
        va_start(args, fmt);
        success = appendStringInfoHugeVA(str, fmt, args);
        va_end(args);

        if (success) {
            break;
		}

        /* Double the buffer size and try again. */
        enlargeStringInfoHuge(str, str->maxlen);
    }
}

