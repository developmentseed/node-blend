#ifndef NODE_BLEND_SRC_MINIZ_PNG_HPP
#define NODE_BLEND_SRC_MINIZ_PNG_HPP

// blend
#include "image_data.hpp"
#include "palette.hpp"

// stl
#include <vector>
#include <iostream>
#include <stdexcept>

// miniz
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.c"

namespace MiniZ {

// TODO
// - support for grayscale images

class PNGWriter {

public:
    PNGWriter(int level = MZ_DEFAULT_COMPRESSION) {
        buffer.m_pBuf = NULL;
        compressor = NULL;

        if (level == -1) {
            level = 6;
        } else if (level < 0 || level > 10) {
            throw std::runtime_error("compression level must be between 0 and 10");
        }
        flags = s_tdefl_num_probes[level] | TDEFL_GREEDY_PARSING_FLAG | TDEFL_WRITE_ZLIB_HEADER;

        buffer.m_capacity = 8192;
        buffer.m_expandable = MZ_TRUE;
        buffer.m_pBuf = (mz_uint8 *)MZ_MALLOC(buffer.m_capacity);
        if (buffer.m_pBuf == NULL) throw std::bad_alloc();

        compressor = (tdefl_compressor *)MZ_MALLOC(sizeof(tdefl_compressor));
        if (compressor == NULL) throw std::bad_alloc();

        // Reset output buffer.
        buffer.m_size = 0;
        tdefl_init(compressor, tdefl_output_buffer_putter, &buffer, flags);

        // Write preamble.
        mz_bool status = tdefl_output_buffer_putter(preamble, 8, &buffer);
        if (status != MZ_TRUE) throw std::bad_alloc();
    }

    ~PNGWriter() {
        if (compressor) {
            MZ_FREE(compressor);
        }
        if (buffer.m_pBuf) {
            MZ_FREE(buffer.m_pBuf);
        }
    }

private:
    inline void writeUInt32BE(mz_uint8 *target, mz_uint32 const& value) {
        target[0] = (value >> 24) & 0xFF;
        target[1] = (value >> 16) & 0xFF;
        target[2] = (value >> 8) & 0xFF;
        target[3] = value & 0xFF;
    }

    size_t startChunk(const mz_uint8 header[], size_t length) {
        size_t start = buffer.m_size;
        mz_bool status = tdefl_output_buffer_putter(header, length, &buffer);
        if (status != MZ_TRUE) throw std::bad_alloc();
        return start;
    }

    void finishChunk(size_t start) {
        // Write chunk length at the beginning of the chunk.
        size_t payloadLength = buffer.m_size - start - 4 - 4;
        writeUInt32BE(buffer.m_pBuf + start, payloadLength);

        // Write CRC32 checksum. Don't include the 4-byte length, but /do/ include
        // the 4-byte chunk name.
        mz_uint32 crc = mz_crc32(MZ_CRC32_INIT, buffer.m_pBuf + start + 4, payloadLength + 4);
        mz_uint8 checksum[] = { crc >> 24, crc >> 16, crc >> 8, crc };
        mz_bool status = tdefl_output_buffer_putter(checksum, 4, &buffer);
        if (status != MZ_TRUE) throw std::bad_alloc();
    }

public:
    void writeIHDR(mz_uint32 width, mz_uint32 height, mz_uint8 pixel_depth) {
        // Write IHDR chunk.
        size_t IHDR = startChunk(IHDR_tpl, 21);
        writeUInt32BE(buffer.m_pBuf + IHDR + 8, width);
        writeUInt32BE(buffer.m_pBuf + IHDR + 12, height);

        if (pixel_depth == 32) {
            // Alpha full color image.
            buffer.m_pBuf[IHDR + 16] = 8; // bit depth
            buffer.m_pBuf[IHDR + 17] = 6; // color type (6 == true color with alpha)
        } else if (pixel_depth == 24) {
            // Full color image.
            buffer.m_pBuf[IHDR + 16] = 8; // bit depth
            buffer.m_pBuf[IHDR + 17] = 2; // color type (2 == true color without alpha)
        } else {
            // Paletted image.
            buffer.m_pBuf[IHDR + 16] = pixel_depth; // bit depth
            buffer.m_pBuf[IHDR + 17] = 3; // color type (3 == indexed color)
        }

        buffer.m_pBuf[IHDR + 18] = 0; // compression method
        buffer.m_pBuf[IHDR + 19] = 0; // filter method
        buffer.m_pBuf[IHDR + 20] = 0; // interlace method
        finishChunk(IHDR);
    }

    void writePLTE(std::vector<rgb> const& palette) {
        // Write PLTE chunk.
        size_t PLTE = startChunk(PLTE_tpl, 8);
        mz_uint8 *colors = const_cast<mz_uint8 *>(reinterpret_cast<const mz_uint8 *>(&palette[0]));
        mz_bool status = tdefl_output_buffer_putter(colors, palette.size() * 3, &buffer);
        if (status != MZ_TRUE) throw std::bad_alloc();
        finishChunk(PLTE);
    }

    void writetRNS(std::vector<unsigned> const& alpha) {
        if (alpha.size() == 0) return;

        std::vector<unsigned char> transparency(alpha.size());
        unsigned char transparencySize = 0; // Stores position of biggest to nonopaque value.
        for(unsigned i = 0; i < alpha.size(); i++) {
            transparency[i] = alpha[i];
            if (alpha[i] < 255) transparencySize = i + 1;
        }
        if (transparencySize > 0) {
            // Write tRNS chunk.
            size_t tRNS = startChunk(tRNS_tpl, 8);
            mz_bool status = tdefl_output_buffer_putter(&transparency[0], transparencySize, &buffer);
            if (status != MZ_TRUE) throw std::bad_alloc();
            finishChunk(tRNS);
        }
    }

    template<typename T>
    void writeIDAT(T const& image) {
        // Write IDAT chunk.
        size_t IDAT = startChunk(IDAT_tpl, 8);
        mz_uint8 filter_type = 0;
        tdefl_status status;

        int bytes_per_pixel = sizeof(typename T::pixel_type);
        int stride = image.width() * bytes_per_pixel;

        for (unsigned int y = 0; y < image.height(); y++) {
            // Write filter_type
            status = tdefl_compress_buffer(compressor, &filter_type, 1, TDEFL_NO_FLUSH);
            if (status != TDEFL_STATUS_OKAY) throw std::runtime_error("failed to compress image");

            // Write scanline
            status = tdefl_compress_buffer(compressor, (mz_uint8 *)image.getRow(y), stride, TDEFL_NO_FLUSH);
            if (status != TDEFL_STATUS_OKAY) throw std::runtime_error("failed to compress image");
        }

        status = tdefl_compress_buffer(compressor, NULL, 0, TDEFL_FINISH);
        if (status != TDEFL_STATUS_DONE) throw std::runtime_error("failed to compress image");

        finishChunk(IDAT);
    }

    void writeIDATStripAlpha(image_data_32 const& image) {
        // Write IDAT chunk.
        size_t IDAT = startChunk(IDAT_tpl, 8);
        mz_uint8 filter_type = 0;
        tdefl_status status;

        size_t stride = image.width() * 3;
        size_t i, j;
        mz_uint8 *scanline = (mz_uint8 *)MZ_MALLOC(stride);

        for (unsigned int y = 0; y < image.height(); y++) {
            // Write filter_type
            status = tdefl_compress_buffer(compressor, &filter_type, 1, TDEFL_NO_FLUSH);
            if (status != TDEFL_STATUS_OKAY) {
                MZ_FREE(scanline);
                throw std::runtime_error("failed to compress image");
            }

            // Strip alpha bytes from scanline
            mz_uint8 *row = (mz_uint8 *)image.getRow(y);
            for (i = 0, j = 0; j < stride; i += 4, j += 3) {
                scanline[j] = row[i];
                scanline[j+1] = row[i+1];
                scanline[j+2] = row[i+2];
            }

            // Write scanline
            status = tdefl_compress_buffer(compressor, scanline, stride, TDEFL_NO_FLUSH);
            if (status != TDEFL_STATUS_OKAY) {
                MZ_FREE(scanline);
                throw std::runtime_error("failed to compress image");
            }
        }

        MZ_FREE(scanline);

        status = tdefl_compress_buffer(compressor, NULL, 0, TDEFL_FINISH);
        if (status != TDEFL_STATUS_DONE) throw std::runtime_error("failed to compress image");

        finishChunk(IDAT);
    }

    void writeIEND() {
        // Write IEND chunk.
        size_t IEND = startChunk(IEND_tpl, 8);
        finishChunk(IEND);
    }

    void toStream(std::ostream& stream) {
        stream.write((char *)buffer.m_pBuf, buffer.m_size);
    }

private:
    mz_uint flags;
    tdefl_compressor *compressor;
    tdefl_output_buffer buffer;

    static const mz_uint8 preamble[];
    static const mz_uint8 IHDR_tpl[];
    static const mz_uint8 PLTE_tpl[];
    static const mz_uint8 tRNS_tpl[];
    static const mz_uint8 IDAT_tpl[];
    static const mz_uint8 IEND_tpl[];
};

const mz_uint8 PNGWriter::preamble[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

const mz_uint8 PNGWriter::IHDR_tpl[] = {
    0x00, 0x00, 0x00, 0x0D, // chunk length
    'I', 'H', 'D', 'R',     // "IHDR"
    0x00, 0x00, 0x00, 0x00, // image width (4 bytes)
    0x00, 0x00, 0x00, 0x00, // image height (4 bytes)
    0x00,                   // bit depth (1 byte)
    0x00,                   // color type (1 byte)
    0x00,                   // compression method (1 byte), has to be 0
    0x00,                   // filter method (1 byte)
    0x00                    // interlace method (1 byte)
};

const mz_uint8 PNGWriter::PLTE_tpl[] = {
    0x00, 0x00, 0x00, 0x00, // chunk length
    'P', 'L', 'T', 'E'      // "IDAT"
};

const mz_uint8 PNGWriter::tRNS_tpl[] = {
    0x00, 0x00, 0x00, 0x00, // chunk length
    't', 'R', 'N', 'S'      // "IDAT"
};

const mz_uint8 PNGWriter::IDAT_tpl[] = {
    0x00, 0x00, 0x00, 0x00, // chunk length
    'I', 'D', 'A', 'T'      // "IDAT"
};

const mz_uint8 PNGWriter::IEND_tpl[] = {
    0x00, 0x00, 0x00, 0x00, // chunk length
    'I', 'E', 'N', 'D'      // "IEND"
};

}

#endif
