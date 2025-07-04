/* Copyright (c) 2016-2021 the Civetweb developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

static int
url_encoded_field_found(const struct mg_connection* conn,
    const char* key,
    size_t key_len,
    const char* filename,
    size_t filename_len,
    char* path,
    size_t path_len,
    struct mg_form_data_handler* fdh)
{
    char key_dec[1024];
    char filename_dec[1024];
    int key_dec_len;
    int filename_dec_len;
    int ret;

    key_dec_len =
        mg_url_decode(key, (int)key_len, key_dec, (int)sizeof(key_dec), 1);

    if (((size_t)key_dec_len >= (size_t)sizeof(key_dec)) || (key_dec_len < 0)) {
        return MG_FORM_FIELD_STORAGE_SKIP;
    }

    if (filename) {
        filename_dec_len = mg_url_decode(filename,
            (int)filename_len,
            filename_dec,
            (int)sizeof(filename_dec),
            1);

        if (((size_t)filename_dec_len >= (size_t)sizeof(filename_dec))
            || (filename_dec_len < 0)) {
            /* Log error message and skip this field. */
            mg_cry_internal(conn, "%s: Cannot decode filename", __func__);
            return MG_FORM_FIELD_STORAGE_SKIP;
        }
        remove_dot_segments(filename_dec);

    }
    else {
        filename_dec[0] = 0;
    }

    ret =
        fdh->field_found(key_dec, filename_dec, path, path_len, fdh->user_data);

    if ((ret & 0xF) == MG_FORM_FIELD_STORAGE_GET) {
        if (fdh->field_get == NULL) {
            mg_cry_internal(conn,
                "%s: Function \"Get\" not available",
                __func__);
            return MG_FORM_FIELD_STORAGE_SKIP;
        }
    }
    if ((ret & 0xF) == MG_FORM_FIELD_STORAGE_STORE) {
        if (fdh->field_store == NULL) {
            mg_cry_internal(conn,
                "%s: Function \"Store\" not available",
                __func__);
            return MG_FORM_FIELD_STORAGE_SKIP;
        }
    }

    return ret;
}

static int
url_encoded_field_get(
    const struct mg_connection* conn,
    const char* key,
    size_t key_len,
    const char* value,
    size_t* value_len, /* IN: number of bytes available in "value", OUT: number
                          of bytes processed */
    struct mg_form_data_handler* fdh)
{
    char key_dec[1024];

    char* value_dec = (char*)mg_malloc_ctx(*value_len + 1, conn->phys_ctx);
    int value_dec_len, ret;

    if (!value_dec) {
        /* Log error message and stop parsing the form data. */
        mg_cry_internal(conn,
            "%s: Not enough memory (required: %lu)",
            __func__,
            (unsigned long)(*value_len + 1));
        return MG_FORM_FIELD_STORAGE_ABORT;
    }

    mg_url_decode(key, (int)key_len, key_dec, (int)sizeof(key_dec), 1);

    if (*value_len >= 2 && value[*value_len - 2] == '%')
        *value_len -= 2;
    else if (*value_len >= 1 && value[*value_len - 1] == '%')
        (*value_len)--;
    value_dec_len = mg_url_decode(
        value, (int)*value_len, value_dec, ((int)*value_len) + 1, 1);

    ret = fdh->field_get(key_dec,
        value_dec,
        (size_t)value_dec_len,
        fdh->user_data);

    mg_free(value_dec);

    return ret;
}

static int
unencoded_field_get(const struct mg_connection* conn,
    const char* key,
    size_t key_len,
    const char* value,
    size_t value_len,
    struct mg_form_data_handler* fdh)
{
    char key_dec[1024];
    (void)conn;

    mg_url_decode(key, (int)key_len, key_dec, (int)sizeof(key_dec), 1);

    return fdh->field_get(key_dec, value, value_len, fdh->user_data);
}

static int
field_stored(const struct mg_connection* conn,
    const char* path,
    long long file_size,
    struct mg_form_data_handler* fdh)
{
    /* Equivalent to "upload" callback of "mg_upload". */

    (void)conn; /* we do not need mg_cry here, so conn is currently unused */

    return fdh->field_store(path, file_size, fdh->user_data);
}

static const char*
search_boundary(const char* buf,
    size_t buf_len,
    const char* boundary,
    size_t boundary_len)
{
    char* boundary_start = "\r\n--";
    size_t boundary_start_len = strlen(boundary_start);

    /* We must do a binary search here, not a string search, since the
     * buffer may contain '\x00' bytes, if binary data is transferred. */
    int clen = (int)buf_len - (int)boundary_len - boundary_start_len;
    int i;

    for (i = 0; i <= clen; i++) {
        if (!memcmp(buf + i, boundary_start, boundary_start_len)) {
            if (!memcmp(buf + i + boundary_start_len, boundary, boundary_len)) {
                return buf + i;
            }
        }
    }
    return NULL;
}

int
mg_handle_form_request(struct mg_connection* conn,
    struct mg_form_data_handler* fdh)
{
    const char* content_type;
    char path[512];
    char buf[MG_BUF_LEN]; /* Must not be smaller than ~900 */
    int field_storage;
    size_t buf_fill = 0;
    int r;
    int field_count = 0;
    struct mg_file fstore = STRUCT_FILE_INITIALIZER;
    int64_t file_size = 0; /* init here, to a avoid a false positive
                             "uninitialized variable used" warning */

    int has_body_data =
        (conn->request_info.content_length > 0) || (conn->is_chunked);

    /* Unused without filesystems */
    (void)fstore;
    (void)file_size;

    /* There are three ways to encode data from a HTML form:
     * 1) method: GET (default)
     *    The form data is in the HTTP query string.
     * 2) method: POST, enctype: "application/x-www-form-urlencoded"
     *    The form data is in the request body.
     *    The body is url encoded (the default encoding for POST).
     * 3) method: POST, enctype: "multipart/form-data".
     *    The form data is in the request body of a multipart message.
     *    This is the typical way to handle file upload from a form.
     */

    if (!has_body_data) {
        const char* data;

        if (0 != strcmp(conn->request_info.request_method, "GET")) {
            /* No body data, but not a GET request.
             * This is not a valid form request. */
            return -1;
        }

        /* GET request: form data is in the query string. */
        /* The entire data has already been loaded, so there is no need to
         * call mg_read. We just need to split the query string into key-value
         * pairs. */
        data = conn->request_info.query_string;
        if (!data) {
            /* No query string. */
            return -1;
        }

        /* Split data in a=1&b=xy&c=3&c=4 ... */
        while (*data) {
            const char* val = strchr(data, '=');
            const char* next;
            ptrdiff_t keylen, vallen;

            if (!val) {
                break;
            }
            keylen = val - data;

            /* In every "field_found" callback we ask what to do with the
             * data ("field_storage"). This could be:
             * MG_FORM_FIELD_STORAGE_SKIP (0):
             *   ignore the value of this field
             * MG_FORM_FIELD_STORAGE_GET (1):
             *   read the data and call the get callback function
             * MG_FORM_FIELD_STORAGE_STORE (2):
             *   store the data in a file
             * MG_FORM_FIELD_STORAGE_READ (3):
             *   let the user read the data (for parsing long data on the fly)
             * MG_FORM_FIELD_STORAGE_ABORT (flag):
             *   stop parsing
             */
            memset(path, 0, sizeof(path));
            field_count++;
            field_storage = url_encoded_field_found(conn,
                data,
                (size_t)keylen,
                NULL,
                0,
                path,
                sizeof(path) - 1,
                fdh);

            val++;
            next = strchr(val, '&');
            if (next) {
                vallen = next - val;
            }
            else {
                vallen = (ptrdiff_t)strlen(val);
            }

            if (field_storage == MG_FORM_FIELD_STORAGE_GET) {
                /* Call callback */
                r = url_encoded_field_get(
                    conn, data, (size_t)keylen, val, (size_t*)&vallen, fdh);
                if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                    /* Stop request handling */
                    break;
                }
                if (r == MG_FORM_FIELD_HANDLE_NEXT) {
                    /* Skip to next field */
                    field_storage = MG_FORM_FIELD_STORAGE_SKIP;
                }
            }

            if (next) {
                next++;
            }
            else {
                /* vallen may have been modified by url_encoded_field_get */
                next = val + vallen;
            }

#if !defined(NO_FILESYSTEMS)
            if (field_storage == MG_FORM_FIELD_STORAGE_STORE) {
                /* Store the content to a file */
                if (mg_fopen(conn, path, MG_FOPEN_MODE_WRITE, &fstore) == 0) {
                    fstore.access.fp = NULL;
                }
                file_size = 0;
                if (fstore.access.fp != NULL) {
                    size_t n = (size_t)
                        fwrite(val, 1, (size_t)vallen, fstore.access.fp);
                    if ((n != (size_t)vallen) || (ferror(fstore.access.fp))) {
                        mg_cry_internal(conn,
                            "%s: Cannot write file %s",
                            __func__,
                            path);
                        (void)mg_fclose(&fstore.access);
                        remove_bad_file(conn, path);
                    }
                    file_size += (int64_t)n;

                    if (fstore.access.fp) {
                        r = mg_fclose(&fstore.access);
                        if (r == 0) {
                            /* stored successfully */
                            r = field_stored(conn, path, file_size, fdh);
                            if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                                /* Stop request handling */
                                break;
                            }

                        }
                        else {
                            mg_cry_internal(conn,
                                "%s: Error saving file %s",
                                __func__,
                                path);
                            remove_bad_file(conn, path);
                        }
                        fstore.access.fp = NULL;
                    }

                }
                else {
                    mg_cry_internal(conn,
                        "%s: Cannot create file %s",
                        __func__,
                        path);
                }
            }
#endif /* NO_FILESYSTEMS */

            /* if (field_storage == MG_FORM_FIELD_STORAGE_READ) { */
            /* The idea of "field_storage=read" is to let the API user read
             * data chunk by chunk and to some data processing on the fly.
             * This should avoid the need to store data in the server:
             * It should neither be stored in memory, like
             * "field_storage=get" does, nor in a file like
             * "field_storage=store".
             * However, for a "GET" request this does not make any much
             * sense, since the data is already stored in memory, as it is
             * part of the query string.
             */
             /* } */

            if ((field_storage & MG_FORM_FIELD_STORAGE_ABORT)
                == MG_FORM_FIELD_STORAGE_ABORT) {
                /* Stop parsing the request */
                break;
            }

            /* Proceed to next entry */
            data = next;
        }

        return field_count;
    }

    content_type = mg_get_header(conn, "Content-Type");

    if (!content_type
        || !mg_strncasecmp(content_type,
            "APPLICATION/X-WWW-FORM-URLENCODED",
            33)
        || !mg_strncasecmp(content_type,
            "APPLICATION/WWW-FORM-URLENCODED",
            31)) {
        /* The form data is in the request body data, encoded in key/value
         * pairs. */
        int all_data_read = 0;

        /* Read body data and split it in keys and values.
         * The encoding is like in the "GET" case above: a=1&b&c=3&c=4.
         * Here we use "POST", and read the data from the request body.
         * The data read on the fly, so it is not required to buffer the
         * entire request in memory before processing it. */
        for (;;) {
            const char* val;
            const char* next;
            ptrdiff_t keylen, vallen;
            ptrdiff_t used;
            int end_of_key_value_pair_found = 0;
            int get_block;

            if (buf_fill < (sizeof(buf) - 1)) {

                size_t to_read = sizeof(buf) - 1 - buf_fill;
                r = mg_read(conn, buf + buf_fill, to_read);
                if ((r < 0) || ((r == 0) && all_data_read)) {
                    /* read error */
                    return -1;
                }
                if (r == 0) {
                    /* TODO: Create a function to get "all_data_read" from
                     * the conn object. All data is read if the Content-Length
                     * has been reached, or if chunked encoding is used and
                     * the end marker has been read, or if the connection has
                     * been closed. */
                    all_data_read = (buf_fill == 0);
                }
                buf_fill += r;
                buf[buf_fill] = 0;
                if (buf_fill < 1) {
                    break;
                }
            }

            val = strchr(buf, '=');

            if (!val) {
                break;
            }
            keylen = val - buf;
            val++;

            /* Call callback */
            memset(path, 0, sizeof(path));
            field_count++;
            field_storage = url_encoded_field_found(conn,
                buf,
                (size_t)keylen,
                NULL,
                0,
                path,
                sizeof(path) - 1,
                fdh);

            if ((field_storage & MG_FORM_FIELD_STORAGE_ABORT)
                == MG_FORM_FIELD_STORAGE_ABORT) {
                /* Stop parsing the request */
                break;
            }

#if !defined(NO_FILESYSTEMS)
            if (field_storage == MG_FORM_FIELD_STORAGE_STORE) {
                if (mg_fopen(conn, path, MG_FOPEN_MODE_WRITE, &fstore) == 0) {
                    fstore.access.fp = NULL;
                }
                file_size = 0;
                if (!fstore.access.fp) {
                    mg_cry_internal(conn,
                        "%s: Cannot create file %s",
                        __func__,
                        path);
                }
            }
#endif /* NO_FILESYSTEMS */

            get_block = 0;
            /* Loop to read values larger than sizeof(buf)-keylen-2 */
            do {
                next = strchr(val, '&');
                if (next) {
                    vallen = next - val;
                    end_of_key_value_pair_found = 1;
                }
                else {
                    vallen = (ptrdiff_t)strlen(val);
                    end_of_key_value_pair_found = all_data_read;
                }

                if (field_storage == MG_FORM_FIELD_STORAGE_GET) {
#if 0
                    if (!end_of_key_value_pair_found && !all_data_read) {
                        /* This callback will deliver partial contents */
                    }
#endif

                    /* Call callback */
                    r = url_encoded_field_get(conn,
                        ((get_block > 0) ? NULL : buf),
                        ((get_block > 0)
                            ? 0
                            : (size_t)keylen),
                        val,
                        (size_t*)&vallen,
                        fdh);
                    get_block++;
                    if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                        /* Stop request handling */
                        break;
                    }
                    if (r == MG_FORM_FIELD_HANDLE_NEXT) {
                        /* Skip to next field */
                        field_storage = MG_FORM_FIELD_STORAGE_SKIP;
                    }
                }

                if (next) {
                    next++;
                }
                else {
                    /* vallen may have been modified by url_encoded_field_get */
                    next = val + vallen;
                }

#if !defined(NO_FILESYSTEMS)
                if (fstore.access.fp) {
                    size_t n = (size_t)
                        fwrite(val, 1, (size_t)vallen, fstore.access.fp);
                    if ((n != (size_t)vallen) || (ferror(fstore.access.fp))) {
                        mg_cry_internal(conn,
                            "%s: Cannot write file %s",
                            __func__,
                            path);
                        mg_fclose(&fstore.access);
                        remove_bad_file(conn, path);
                    }
                    file_size += (int64_t)n;
                }
#endif /* NO_FILESYSTEMS */

                if (!end_of_key_value_pair_found) {
                    used = next - buf;
                    memmove(buf,
                        buf + (size_t)used,
                        sizeof(buf) - (size_t)used);
                    next = buf;
                    buf_fill -= used;
                    if (buf_fill < (sizeof(buf) - 1)) {

                        size_t to_read = sizeof(buf) - 1 - buf_fill;
                        r = mg_read(conn, buf + buf_fill, to_read);
                        if ((r < 0) || ((r == 0) && all_data_read)) {
#if !defined(NO_FILESYSTEMS)
                            /* read error */
                            if (fstore.access.fp) {
                                mg_fclose(&fstore.access);
                                remove_bad_file(conn, path);
                            }
                            return -1;
#endif /* NO_FILESYSTEMS */
                        }
                        if (r == 0) {
                            /* TODO: Create a function to get "all_data_read"
                             * from the conn object. All data is read if the
                             * Content-Length has been reached, or if chunked
                             * encoding is used and the end marker has been
                             * read, or if the connection has been closed. */
                            all_data_read = (buf_fill == 0);
                        }
                        buf_fill += r;
                        buf[buf_fill] = 0;
                        if (buf_fill < 1) {
                            break;
                        }
                        val = buf;
                    }
                }

            } while (!end_of_key_value_pair_found);

#if !defined(NO_FILESYSTEMS)
            if (fstore.access.fp) {
                r = mg_fclose(&fstore.access);
                if (r == 0) {
                    /* stored successfully */
                    r = field_stored(conn, path, file_size, fdh);
                    if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                        /* Stop request handling */
                        break;
                    }
                }
                else {
                    mg_cry_internal(conn,
                        "%s: Error saving file %s",
                        __func__,
                        path);
                    remove_bad_file(conn, path);
                }
                fstore.access.fp = NULL;
            }
#endif /* NO_FILESYSTEMS */

            if (all_data_read && (buf_fill == 0)) {
                /* nothing more to process */
                break;
            }

            /* Proceed to next entry */
            used = next - buf;
            memmove(buf, buf + (size_t)used, sizeof(buf) - (size_t)used);
            buf_fill -= used;
        }

        return field_count;
    }

    if (!mg_strncasecmp(content_type, "MULTIPART/FORM-DATA;", 20)) {
        /* The form data is in the request body data, encoded as multipart
         * content (see https://www.ietf.org/rfc/rfc1867.txt,
         * https://www.ietf.org/rfc/rfc2388.txt). */
        char* boundary;
        size_t bl;
        ptrdiff_t used;
        struct mg_request_info part_header;
        char* hbuf;
        const char* content_disp, * hend, * fbeg, * fend, * nbeg, * nend;
        const char* next;
        unsigned part_no;
        int all_data_read = 0;

        memset(&part_header, 0, sizeof(part_header));

        /* Skip all spaces between MULTIPART/FORM-DATA; and BOUNDARY= */
        bl = 20;
        while (content_type[bl] == ' ') {
            bl++;
        }

        /* There has to be a BOUNDARY definition in the Content-Type header */
        if (mg_strncasecmp(content_type + bl, "BOUNDARY=", 9)) {
            /* Malformed request */
            return -1;
        }

        /* Copy boundary string to variable "boundary" */
        /* fbeg is pointer to start of value of boundary */
        fbeg = content_type + bl + 9;
        bl = strlen(fbeg);
        boundary = (char*)mg_malloc(bl + 1);
        if (!boundary) {
            /* Out of memory */
            mg_cry_internal(conn,
                "%s: Cannot allocate memory for boundary [%lu]",
                __func__,
                (unsigned long)bl);
            return -1;
        }
        memcpy(boundary, fbeg, bl);
        boundary[bl] = 0;

        /* RFC 2046 permits the boundary string to be quoted. */
        /* If the boundary is quoted, trim the quotes */
        if (boundary[0] == '"') {
            hbuf = strchr(boundary + 1, '"');
            if ((!hbuf) || (*hbuf != '"')) {
                /* Malformed request */
                mg_free(boundary);
                return -1;
            }
            *hbuf = 0;
            memmove(boundary, boundary + 1, bl);
            bl = strlen(boundary);
        }

        /* Do some sanity checks for boundary lengths */
        if (bl > 70) {
            /* From RFC 2046:
             * Boundary delimiters must not appear within the
             * encapsulated material, and must be no longer
             * than 70 characters, not counting the two
             * leading hyphens.
             */

             /* The algorithm can not work if bl >= sizeof(buf), or if buf
              * can not hold the multipart header plus the boundary.
              * Requests with long boundaries are not RFC compliant, maybe they
              * are intended attacks to interfere with this algorithm. */
            mg_free(boundary);
            return -1;
        }
        if (bl < 4) {
            /* Sanity check:  A boundary string of less than 4 bytes makes
             * no sense either. */
            mg_free(boundary);
            return -1;
        }

        for (part_no = 0;; part_no++) {
            size_t towrite, fnlen, n;
            int get_block;
            size_t to_read = sizeof(buf) - 1 - buf_fill;

            /* Unused without filesystems */
            (void)n;

            r = mg_read(conn, buf + buf_fill, to_read);
            if ((r < 0) || ((r == 0) && all_data_read)) {
                /* read error */
                mg_free(boundary);
                return -1;
            }
            if (r == 0) {
                all_data_read = (buf_fill == 0);
            }

            buf_fill += r;
            buf[buf_fill] = 0;
            if (buf_fill < 1) {
                /* No data */
                mg_free(boundary);
                return -1;
            }

            /* @see https://www.rfc-editor.org/rfc/rfc2046.html#section-5.1.1
             *
             * multipart-body := [preamble CRLF]
             *     dash-boundary transport-padding CRLF
             *     body-part *encapsulation
             *     close-delimiter transport-padding
             *     [CRLF epilogue]
             */

            if (part_no == 0) {
                size_t preamble_length = 0;
                /* skip over the preamble until we find a complete boundary
                 * limit the preamble length to prevent abuse */
                 /* +2 for the -- preceding the boundary */
                while (preamble_length < 1024
                    && (preamble_length < buf_fill - bl)
                    && strncmp(buf + preamble_length + 2, boundary, bl)) {
                    preamble_length++;
                }
                /* reset the start of buf to remove the preamble */
                if (0 == strncmp(buf + preamble_length + 2, boundary, bl)) {
                    memmove(buf,
                        buf + preamble_length,
                        (unsigned)buf_fill - (unsigned)preamble_length);
                    buf_fill -= preamble_length;
                    buf[buf_fill] = 0;
                }
            }

            /* either it starts with a boundary and it's fine, or it's malformed
             * because:
             * - the preamble was longer than accepted
             * - couldn't find a boundary at all in the body
             * - didn't have a terminating boundary */
            if (buf_fill < (bl + 2) || strncmp(buf, "--", 2)
                || strncmp(buf + 2, boundary, bl)) {
                /* Malformed request */
                mg_free(boundary);
                return -1;
            }

            /* skip the -- */
            char* boundary_start = buf + 2;
            size_t transport_padding = 0;
            while (boundary_start[bl + transport_padding] == ' '
                || boundary_start[bl + transport_padding] == '\t') {
                transport_padding++;
            }
            char* boundary_end = boundary_start + bl + transport_padding;

            /* after the transport padding, if the boundary isn't
             * immediately followed by a \r\n then it is either... */
            if (strncmp(boundary_end, "\r\n", 2)) {
                /* ...the final boundary, and it is followed by --, (in which
                 * case it's the end of the request) or it's a malformed
                 * request */
                if (strncmp(boundary_end, "--", 2)) {
                    /* Malformed request */
                    mg_free(boundary);
                    return -1;
                }
                /* Ingore any epilogue here */
                break;
            }

            /* skip the \r\n */
            hbuf = boundary_end + 2;
            /* Next, we need to get the part header: Read until \r\n\r\n */
            hend = strstr(hbuf, "\r\n\r\n");
            if (!hend) {
                /* Malformed request */
                mg_free(boundary);
                return -1;
            }

            part_header.num_headers =
                parse_http_headers(&hbuf, part_header.http_headers);
            if ((hend + 2) != hbuf) {
                /* Malformed request */
                mg_free(boundary);
                return -1;
            }

            /* Skip \r\n\r\n */
            hend += 4;

            /* According to the RFC, every part has to have a header field like:
             * Content-Disposition: form-data; name="..." */
            content_disp = get_header(part_header.http_headers,
                part_header.num_headers,
                "Content-Disposition");
            if (!content_disp) {
                /* Malformed request */
                mg_free(boundary);
                return -1;
            }

            /* Get the mandatory name="..." part of the Content-Disposition
             * header. */
            nbeg = strstr(content_disp, "name=\"");
            while ((nbeg != NULL) && (strcspn(nbeg - 1, ":,; \t") != 0)) {
                /* It could be somethingname= instead of name= */
                nbeg = strstr(nbeg + 1, "name=\"");
            }

            /* This line is not required, but otherwise some compilers
             * generate spurious warnings. */
            nend = nbeg;
            /* And others complain, the result is unused. */
            (void)nend;

            /* If name=" is found, search for the closing " */
            if (nbeg) {
                nbeg += 6;
                nend = strchr(nbeg, '\"');
                if (!nend) {
                    /* Malformed request */
                    mg_free(boundary);
                    return -1;
                }
            }
            else {
                /* name= without quotes is also allowed */
                nbeg = strstr(content_disp, "name=");
                while ((nbeg != NULL) && (strcspn(nbeg - 1, ":,; \t") != 0)) {
                    /* It could be somethingname= instead of name= */
                    nbeg = strstr(nbeg + 1, "name=");
                }
                if (!nbeg) {
                    /* Malformed request */
                    mg_free(boundary);
                    return -1;
                }
                nbeg += 5;

                /* RFC 2616 Sec. 2.2 defines a list of allowed
                 * separators, but many of them make no sense
                 * here, e.g. various brackets or slashes.
                 * If they are used, probably someone is
                 * trying to attack with curious hand made
                 * requests. Only ; , space and tab seem to be
                 * reasonable here. Ignore everything else. */
                nend = nbeg + strcspn(nbeg, ",; \t");
            }

            /* Get the optional filename="..." part of the Content-Disposition
             * header. */
            fbeg = strstr(content_disp, "filename=\"");
            while ((fbeg != NULL) && (strcspn(fbeg - 1, ":,; \t") != 0)) {
                /* It could be somethingfilename= instead of filename= */
                fbeg = strstr(fbeg + 1, "filename=\"");
            }

            /* This line is not required, but otherwise some compilers
             * generate spurious warnings. */
            fend = fbeg;

            /* If filename=" is found, search for the closing " */
            if (fbeg) {
                fbeg += 10;
                fend = strchr(fbeg, '\"');

                if (!fend) {
                    /* Malformed request (the filename field is optional, but if
                     * it exists, it needs to be terminated correctly). */
                    mg_free(boundary);
                    return -1;
                }

                /* TODO: check Content-Type */
                /* Content-Type: application/octet-stream */
            }
            if (!fbeg) {
                /* Try the same without quotes */
                fbeg = strstr(content_disp, "filename=");
                while ((fbeg != NULL) && (strcspn(fbeg - 1, ":,; \t") != 0)) {
                    /* It could be somethingfilename= instead of filename= */
                    fbeg = strstr(fbeg + 1, "filename=");
                }
                if (fbeg) {
                    fbeg += 9;
                    fend = fbeg + strcspn(fbeg, ",; \t");
                }
            }

            if (!fbeg || !fend) {
                fbeg = NULL;
                fend = NULL;
                fnlen = 0;
            }
            else {
                fnlen = (size_t)(fend - fbeg);
            }

            /* In theory, it could be possible that someone crafts
             * a request like name=filename=xyz. Check if name and
             * filename do not overlap. */
            if (!(((ptrdiff_t)fbeg > (ptrdiff_t)nend)
                || ((ptrdiff_t)nbeg > (ptrdiff_t)fend))) {
                mg_free(boundary);
                return -1;
            }

            /* Call callback for new field */
            memset(path, 0, sizeof(path));
            field_count++;
            field_storage = url_encoded_field_found(conn,
                nbeg,
                (size_t)(nend - nbeg),
                ((fnlen > 0) ? fbeg : NULL),
                fnlen,
                path,
                sizeof(path) - 1,
                fdh);

            /* If the boundary is already in the buffer, get the address,
             * otherwise next will be NULL. */
            next = search_boundary(hbuf,
                (size_t)((buf - hbuf) + buf_fill),
                boundary,
                bl);

#if !defined(NO_FILESYSTEMS)
            if (field_storage == MG_FORM_FIELD_STORAGE_STORE) {
                /* Store the content to a file */
                if (mg_fopen(conn, path, MG_FOPEN_MODE_WRITE, &fstore) == 0) {
                    fstore.access.fp = NULL;
                }
                file_size = 0;

                if (!fstore.access.fp) {
                    mg_cry_internal(conn,
                        "%s: Cannot create file %s",
                        __func__,
                        path);
                }
            }
#endif /* NO_FILESYSTEMS */

            get_block = 0;
            while (!next) {
                /* Set "towrite" to the number of bytes available
                 * in the buffer */
                towrite = (size_t)(buf - hend + buf_fill);

                if (towrite < bl + 4) {
                    /* Not enough data stored. */
                    /* Incomplete request. */
                    mg_free(boundary);
                    return -1;
                }

                /* Subtract the boundary length, to deal with
                 * cases the boundary is only partially stored
                 * in the buffer. */
                towrite -= bl + 4;

                if (field_storage == MG_FORM_FIELD_STORAGE_GET) {
                    r = unencoded_field_get(conn,
                        ((get_block > 0) ? NULL : nbeg),
                        ((get_block > 0)
                            ? 0
                            : (size_t)(nend - nbeg)),
                        hend,
                        towrite,
                        fdh);
                    get_block++;
                    if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                        /* Stop request handling */
                        break;
                    }
                    if (r == MG_FORM_FIELD_HANDLE_NEXT) {
                        /* Skip to next field */
                        field_storage = MG_FORM_FIELD_STORAGE_SKIP;
                    }
                }

#if !defined(NO_FILESYSTEMS)
                if (field_storage == MG_FORM_FIELD_STORAGE_STORE) {
                    if (fstore.access.fp) {

                        /* Store the content of the buffer. */
                        n = (size_t)fwrite(hend, 1, towrite, fstore.access.fp);
                        if ((n != towrite) || (ferror(fstore.access.fp))) {
                            mg_cry_internal(conn,
                                "%s: Cannot write file %s",
                                __func__,
                                path);
                            mg_fclose(&fstore.access);
                            remove_bad_file(conn, path);
                        }
                        file_size += (int64_t)n;
                    }
                }
#endif /* NO_FILESYSTEMS */

                memmove(buf, hend + towrite, bl + 4);
                buf_fill = bl + 4;
                hend = buf;

                /* Read new data */
                to_read = sizeof(buf) - 1 - buf_fill;
                r = mg_read(conn, buf + buf_fill, to_read);
                if ((r < 0) || ((r == 0) && all_data_read)) {
#if !defined(NO_FILESYSTEMS)
                    /* read error */
                    if (fstore.access.fp) {
                        mg_fclose(&fstore.access);
                        remove_bad_file(conn, path);
                    }
#endif /* NO_FILESYSTEMS */
                    mg_free(boundary);
                    return -1;
                }
                /* r==0 already handled, all_data_read is false here */

                buf_fill += r;
                buf[buf_fill] = 0;
                /* buf_fill is at least 8 here */

                /* Find boundary */
                next = search_boundary(buf, buf_fill, boundary, bl);

                if (!next && (r == 0)) {
                    /* incomplete request */
                    all_data_read = 1;
                }
            }

            towrite = (next ? (size_t)(next - hend) : 0);

            if (field_storage == MG_FORM_FIELD_STORAGE_GET) {
                /* Call callback */
                r = unencoded_field_get(conn,
                    ((get_block > 0) ? NULL : nbeg),
                    ((get_block > 0)
                        ? 0
                        : (size_t)(nend - nbeg)),
                    hend,
                    towrite,
                    fdh);
                if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                    /* Stop request handling */
                    break;
                }
                if (r == MG_FORM_FIELD_HANDLE_NEXT) {
                    /* Skip to next field */
                    field_storage = MG_FORM_FIELD_STORAGE_SKIP;
                }
            }

#if !defined(NO_FILESYSTEMS)
            if (field_storage == MG_FORM_FIELD_STORAGE_STORE) {

                if (fstore.access.fp) {
                    n = (size_t)fwrite(hend, 1, towrite, fstore.access.fp);
                    if ((n != towrite) || (ferror(fstore.access.fp))) {
                        mg_cry_internal(conn,
                            "%s: Cannot write file %s",
                            __func__,
                            path);
                        mg_fclose(&fstore.access);
                        remove_bad_file(conn, path);
                    }
                    else {
                        file_size += (int64_t)n;
                        r = mg_fclose(&fstore.access);
                        if (r == 0) {
                            /* stored successfully */
                            r = field_stored(conn, path, file_size, fdh);
                            if (r == MG_FORM_FIELD_HANDLE_ABORT) {
                                /* Stop request handling */
                                break;
                            }
                        }
                        else {
                            mg_cry_internal(conn,
                                "%s: Error saving file %s",
                                __func__,
                                path);
                            remove_bad_file(conn, path);
                        }
                    }
                    fstore.access.fp = NULL;
                }
            }
#endif /* NO_FILESYSTEMS */

            if ((field_storage & MG_FORM_FIELD_STORAGE_ABORT)
                == MG_FORM_FIELD_STORAGE_ABORT) {
                /* Stop parsing the request */
                break;
            }

            /* Remove from the buffer */
            if (next) {
                used = next - buf + 2;
                memmove(buf, buf + (size_t)used, sizeof(buf) - (size_t)used);
                buf_fill -= used;
            }
            else {
                buf_fill = 0;
            }
        }

        /* All parts handled */
        mg_free(boundary);
        return field_count;
    }

    /* Unknown Content-Type */
    return -1;
}

/* End of handle_form.inl */