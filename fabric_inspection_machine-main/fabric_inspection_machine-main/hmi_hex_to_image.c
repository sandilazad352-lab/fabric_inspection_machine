#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER "0D0A1B4BF000"
#define MARKER_LEN 12
#define ROW_BYTES 240
#define BITS_PER_BYTE 8

static unsigned char *read_file_bytes(const char *path, long *out_size) {
    FILE *fp = fopen(path, "rb");
    unsigned char *buffer = NULL;
    long size;

    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    buffer = (unsigned char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)size, fp) != (size_t)size) {
        free(buffer);
        fclose(fp);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(fp);
    *out_size = size;
    return buffer;
}

static char *strip_whitespace_copy(const unsigned char *input, long size) {
    char *cleaned = (char *)malloc((size_t)size + 1);
    long write_index = 0;
    long read_index;

    if (!cleaned) {
        return NULL;
    }

    for (read_index = 0; read_index < size; ++read_index) {
        unsigned char ch = input[read_index];
        if (!isspace(ch)) {
            cleaned[write_index++] = (char)ch;
        }
    }
    cleaned[write_index] = '\0';
    return cleaned;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static int parse_hex_byte(const char *text, unsigned char *out_value) {
    int hi = hex_value(text[0]);
    int lo = hex_value(text[1]);
    if (hi < 0 || lo < 0) {
        return 0;
    }
    *out_value = (unsigned char)((hi << 4) | lo);
    return 1;
}

static unsigned char *hex_string_to_bytes(const char *hex_text, long *out_size) {
    size_t len = strlen(hex_text);
    size_t byte_count = len / 2;
    unsigned char *bytes = (unsigned char *)malloc(byte_count);
    size_t i;
    size_t written = 0;

    if (!bytes) {
        return NULL;
    }

    if (len % 2 != 0) {
        free(bytes);
        return NULL;
    }

    for (i = 0; i < len; i += 2) {
        unsigned char value;
        if (!parse_hex_byte(hex_text + i, &value)) {
            free(bytes);
            return NULL;
        }
        bytes[written++] = value;
    }

    *out_size = (long)written;
    return bytes;
}

static unsigned char *build_bitmap_from_hex(const unsigned char *hex_bytes, long hex_size, int *out_width, int *out_height) {
    char *clean_hex = strip_whitespace_copy(hex_bytes, hex_size);
    char *copy_for_search = NULL;
    unsigned char *raw_bytes = NULL;
    unsigned char *image = NULL;
    size_t segment_count = 0;
    size_t segment_index = 0;
    size_t hex_len;
    int width = ROW_BYTES;
    int height = 0;
    size_t x_index;
    size_t bit_index;

    if (!clean_hex) {
        return NULL;
    }

    hex_len = strlen(clean_hex);
    copy_for_search = (char *)malloc(hex_len + 1);
    if (!copy_for_search) {
        free(clean_hex);
        return NULL;
    }
    memcpy(copy_for_search, clean_hex, hex_len + 1);

    /* Count data segments delimited by the known marker. */
    {
        char *cursor = copy_for_search;
        char *found;
        while ((found = strstr(cursor, MARKER)) != NULL) {
            char *segment_start = cursor;
            size_t segment_len = (size_t)(found - segment_start);
            if (segment_len == ROW_BYTES * 2) {
                ++segment_count;
            }
            cursor = found + MARKER_LEN;
        }
    }

    if (segment_count == 0) {
        free(copy_for_search);
        free(clean_hex);
        return NULL;
    }

    raw_bytes = (unsigned char *)malloc(segment_count * ROW_BYTES);
    if (!raw_bytes) {
        free(copy_for_search);
        free(clean_hex);
        return NULL;
    }

    {
        char *cursor = copy_for_search;
        char *found;
        while ((found = strstr(cursor, MARKER)) != NULL) {
            char saved;
            size_t segment_len = (size_t)(found - cursor);
            if (segment_len == ROW_BYTES * 2) {
                char *segment_copy = (char *)malloc(segment_len + 1);
                long byte_size = 0;
                unsigned char *segment_bytes;

                if (!segment_copy) {
                    free(raw_bytes);
                    free(copy_for_search);
                    free(clean_hex);
                    return NULL;
                }
                memcpy(segment_copy, cursor, segment_len);
                segment_copy[segment_len] = '\0';
                segment_bytes = hex_string_to_bytes(segment_copy, &byte_size);
                free(segment_copy);

                if (!segment_bytes || byte_size != ROW_BYTES) {
                    free(segment_bytes);
                    free(raw_bytes);
                    free(copy_for_search);
                    free(clean_hex);
                    return NULL;
                }

                memcpy(raw_bytes + (segment_index * ROW_BYTES), segment_bytes, ROW_BYTES);
                free(segment_bytes);
                ++segment_index;
            }
            saved = *found;
            *found = '\0';
            *found = saved;
            cursor = found + MARKER_LEN;
        }
    }

    height = (int)(segment_index * BITS_PER_BYTE);
    image = (unsigned char *)calloc((size_t)width * (size_t)height, 1);
    if (!image) {
        free(raw_bytes);
        free(copy_for_search);
        free(clean_hex);
        return NULL;
    }

    for (segment_index = 0; segment_index < (size_t)height / BITS_PER_BYTE; ++segment_index) {
        const unsigned char *segment = raw_bytes + (segment_index * ROW_BYTES);
        for (x_index = 0; x_index < ROW_BYTES; ++x_index) {
            size_t draw_x = (size_t)(ROW_BYTES - 1 - x_index);
            unsigned char byte_value = segment[x_index];
            for (bit_index = 0; bit_index < BITS_PER_BYTE; ++bit_index) {
                if (!(byte_value & (1u << bit_index))) {
                    size_t y = (segment_index * BITS_PER_BYTE) + bit_index;
                    image[y * (size_t)width + draw_x] = 1;
                }
            }
        }
    }

    free(raw_bytes);
    free(copy_for_search);
    free(clean_hex);
    *out_width = width;
    *out_height = height;
    return image;
}

static unsigned char *resize_nearest(const unsigned char *src, int src_w, int src_h, int scale, int *out_w, int *out_h) {
    int dst_w = src_w * scale;
    int dst_h = src_h * scale;
    unsigned char *dst = (unsigned char *)calloc((size_t)dst_w * (size_t)dst_h, 1);
    int y;
    int x;
    int src_y;
    int src_x;

    if (!dst) {
        return NULL;
    }

    for (y = 0; y < dst_h; ++y) {
        src_y = y / scale;
        for (x = 0; x < dst_w; ++x) {
            src_x = x / scale;
            dst[y * dst_w + x] = src[src_y * src_w + src_x];
        }
    }

    *out_w = dst_w;
    *out_h = dst_h;
    return dst;
}

static unsigned char *invert_bitmap(const unsigned char *src, int width, int height) {
    unsigned char *dst = (unsigned char *)malloc((size_t)width * (size_t)height);
    int i;
    if (!dst) {
        return NULL;
    }
    for (i = 0; i < width * height; ++i) {
        dst[i] = src[i] ? 0 : 1;
    }
    return dst;
}

static void write_be32(FILE *fp, unsigned long value) {
    fputc((int)((value >> 24) & 0xFF), fp);
    fputc((int)((value >> 16) & 0xFF), fp);
    fputc((int)((value >> 8) & 0xFF), fp);
    fputc((int)(value & 0xFF), fp);
}

static unsigned long crc32_update(unsigned long crc, const unsigned char *data, size_t len) {
    size_t i;
    crc = crc ^ 0xFFFFFFFFUL;
    for (i = 0; i < len; ++i) {
        int bit;
        crc = crc ^ (unsigned long)data[i];
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 1UL) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static unsigned long adler32_sum(const unsigned char *data, size_t len) {
    unsigned long a = 1;
    unsigned long b = 0;
    size_t i;
    for (i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

static int write_chunk(FILE *fp, const char type[4], const unsigned char *data, size_t len) {
    unsigned long crc;
    write_be32(fp, (unsigned long)len);
    if (fwrite(type, 1, 4, fp) != 4) {
        return 0;
    }
    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        return 0;
    }
    crc = crc32_update(0, (const unsigned char *)type, 4);
    if (len > 0) {
        crc = crc32_update(crc, data, len);
    }
    write_be32(fp, crc);
    return 1;
}

static int write_png(const char *path, const unsigned char *bitmap, int width, int height) {
    FILE *fp = NULL;
    unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    unsigned char ihdr[13];
    unsigned char *raw = NULL;
    unsigned char *zlib = NULL;
    size_t raw_size;
    size_t zlib_size;
    size_t zpos;
    size_t offset;
    size_t blocks;
    size_t block_index;
    size_t remaining;
    size_t block_len;
    unsigned long adler;
    int y;
    int x;

    if (width <= 0 || height <= 0) {
        return 0;
    }

    raw_size = (size_t)height * (size_t)(width + 1);
    raw = (unsigned char *)malloc(raw_size);
    if (!raw) {
        return 0;
    }

    for (y = 0; y < height; ++y) {
        size_t row = (size_t)y * (size_t)(width + 1);
        raw[row] = 0;
        for (x = 0; x < width; ++x) {
            raw[row + 1 + (size_t)x] = bitmap[(size_t)y * (size_t)width + (size_t)x] ? 0 : 255;
        }
    }

    blocks = (raw_size + 65535UL - 1UL) / 65535UL;
    zlib_size = 2 + (blocks * 5) + raw_size + 4;
    zlib = (unsigned char *)malloc(zlib_size);
    if (!zlib) {
        free(raw);
        return 0;
    }

    zpos = 0;
    zlib[zpos++] = 0x78;
    zlib[zpos++] = 0x01;

    offset = 0;
    for (block_index = 0; block_index < blocks; ++block_index) {
        remaining = raw_size - offset;
        block_len = remaining > 65535UL ? 65535UL : remaining;
        zlib[zpos++] = (unsigned char)(block_index == blocks - 1 ? 1 : 0);
        zlib[zpos++] = (unsigned char)(block_len & 0xFF);
        zlib[zpos++] = (unsigned char)((block_len >> 8) & 0xFF);
        zlib[zpos++] = (unsigned char)((~block_len) & 0xFF);
        zlib[zpos++] = (unsigned char)(((~block_len) >> 8) & 0xFF);
        memcpy(zlib + zpos, raw + offset, block_len);
        zpos += block_len;
        offset += block_len;
    }

    adler = adler32_sum(raw, raw_size);
    zlib[zpos++] = (unsigned char)((adler >> 24) & 0xFF);
    zlib[zpos++] = (unsigned char)((adler >> 16) & 0xFF);
    zlib[zpos++] = (unsigned char)((adler >> 8) & 0xFF);
    zlib[zpos++] = (unsigned char)(adler & 0xFF);

    fp = fopen(path, "wb");
    if (!fp) {
        free(zlib);
        free(raw);
        return 0;
    }

    if (fwrite(signature, 1, 8, fp) != 8) {
        fclose(fp);
        free(zlib);
        free(raw);
        return 0;
    }

    ihdr[0] = (unsigned char)((width >> 24) & 0xFF);
    ihdr[1] = (unsigned char)((width >> 16) & 0xFF);
    ihdr[2] = (unsigned char)((width >> 8) & 0xFF);
    ihdr[3] = (unsigned char)(width & 0xFF);
    ihdr[4] = (unsigned char)((height >> 24) & 0xFF);
    ihdr[5] = (unsigned char)((height >> 16) & 0xFF);
    ihdr[6] = (unsigned char)((height >> 8) & 0xFF);
    ihdr[7] = (unsigned char)(height & 0xFF);
    ihdr[8] = 8;
    ihdr[9] = 0;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    if (!write_chunk(fp, "IHDR", ihdr, 13)) {
        fclose(fp);
        free(zlib);
        free(raw);
        return 0;
    }
    if (!write_chunk(fp, "IDAT", zlib, zpos)) {
        fclose(fp);
        free(zlib);
        free(raw);
        return 0;
    }
    if (!write_chunk(fp, "IEND", NULL, 0)) {
        fclose(fp);
        free(zlib);
        free(raw);
        return 0;
    }

    fclose(fp);
    free(zlib);
    free(raw);
    return 1;
}

int main(int argc, char **argv) {
    const char *input_path;
    const char *output_path;
    int ocr_ready;
    int scale;
    long file_size;
    unsigned char *file_bytes;
    unsigned char *bitmap;
    unsigned char *scaled;
    unsigned char *final_image;
    int width;
    int height;
    int scaled_w;
    int scaled_h;
    int final_w;
    int final_h;
    int i;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.hex [-o output.png] [--ocr-ready] [--scale N]\n", argv[0]);
        return 1;
    }

    input_path = argv[1];
    output_path = "render_from_hex.png";
    ocr_ready = 0;
    scale = 8;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for %s\n", argv[i]);
                return 1;
            }
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--ocr-ready") == 0) {
            ocr_ready = 1;
        } else if (strcmp(argv[i], "--scale") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --scale\n");
                return 1;
            }
            scale = atoi(argv[++i]);
            if (scale <= 0) {
                scale = 1;
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    file_size = 0;
    file_bytes = read_file_bytes(input_path, &file_size);
    bitmap = NULL;
    scaled = NULL;
    final_image = NULL;
    width = 0;
    height = 0;
    scaled_w = 0;
    scaled_h = 0;
    final_w = 0;
    final_h = 0;

    if (!file_bytes) {
        fprintf(stderr, "Failed to read input file: %s\n", input_path);
        return 1;
    }

    bitmap = build_bitmap_from_hex(file_bytes, file_size, &width, &height);
    free(file_bytes);
    if (!bitmap) {
        fprintf(stderr, "Failed to decode HEX raster data.\n");
        return 1;
    }

    scaled = resize_nearest(bitmap, width, height, scale, &scaled_w, &scaled_h);
    free(bitmap);
    if (!scaled) {
        fprintf(stderr, "Failed to scale bitmap.\n");
        return 1;
    }

    if (ocr_ready) {
        final_image = invert_bitmap(scaled, scaled_w, scaled_h);
        free(scaled);
        if (!final_image) {
            fprintf(stderr, "Failed to invert bitmap.\n");
            return 1;
        }
        final_w = scaled_w;
        final_h = scaled_h;
    } else {
        final_image = scaled;
        final_w = scaled_w;
        final_h = scaled_h;
    }

    if (!write_png(output_path, final_image, final_w, final_h)) {
        fprintf(stderr, "Failed to write output image: %s\n", output_path);
        free(final_image);
        return 1;
    }

    free(final_image);
    printf("Saved: %s\n", output_path);
    return 0;
}
