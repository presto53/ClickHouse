#pragma once

#include <vector>

#include <DB/IO/ReadBufferFromFile.h>
#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/UncompressedCache.h>


namespace DB
{

/** Буфер для чтения из сжатого файла с использованием кэша разжатых блоков.
  * Кэш внешний - передаётся в качестве аргумента в конструктор.
  * Позволяет увеличить производительность в случае, когда часто читаются одни и те же блоки.
  * Недостатки:
  * - в случае, если нужно читать много данных подряд, но из них только часть закэширована, приходится делать seek-и.
  */
class CachedCompressedReadBuffer : public ReadBuffer
{
private:
	const std::string path;
	size_t cur_begin_offset; /// Смещение в сжатом файле, соответствующее working_buffer.begin().
	size_t cur_end_offset; /// Смещение в сжатом файле, соответствующее working_buffer.end().
	UncompressedCache * cache;
	size_t buf_size;

	/// SharedPtr - для ленивой инициализации (только в случае кэш-промаха).
	Poco::SharedPtr<ReadBufferFromFile> in;
	Poco::SharedPtr<CompressedReadBuffer> compressed_in;

	/// Кусок данных из кэша, или кусок считанных данных, который мы положим в кэш.
	UncompressedCache::CellPtr owned_cell;


	bool nextImpl()
	{
		/// Проверим наличие разжатого блока в кэше, захватим владение этим блоком, если он есть.

		cur_begin_offset = cur_end_offset;
		UInt128 key = {0, 0};

		if (cache)
		{
			key = cache->hash(path, cur_begin_offset);
			owned_cell = cache->get(key);
		}
		else
		{
			owned_cell = NULL;
		}

		if (!owned_cell)
		{
			/// Если нет - надо прочитать его из файла.
			if (!compressed_in)
			{
				in = new ReadBufferFromFile(path, buf_size);
				compressed_in = new CompressedReadBuffer(*in);
			}

			in->seek(cur_begin_offset);

			owned_cell = new UncompressedCache::Cell;
			owned_cell->key = key;

			/// Разжимать будем в кусок памяти, который будет в кэше.
			compressed_in->setMemory(owned_cell->data);

			size_t old_count = in->count();
			compressed_in->next();
			owned_cell->compressed_size = in->count() - old_count;

			/// Положим данные в кэш.
			if (cache)
				cache->set(owned_cell);
		}

		if (owned_cell->data.m_size == 0)
			return false;

		internal_buffer = Buffer(owned_cell->data.m_data, owned_cell->data.m_data + owned_cell->data.m_size);
		working_buffer = Buffer(owned_cell->data.m_data, owned_cell->data.m_data + owned_cell->data.m_size);
		pos = working_buffer.begin();

		cur_end_offset += owned_cell->compressed_size;
		return true;
	}

public:
	CachedCompressedReadBuffer(const std::string & path_, UncompressedCache * cache_, size_t buf_size_ = DBMS_DEFAULT_BUFFER_SIZE)
		: ReadBuffer(NULL, 0), path(path_), cur_begin_offset(0), cur_end_offset(0), cache(cache_), buf_size(buf_size_)
	{
	}

	void seek(size_t offset_in_compressed_file, size_t offset_in_decompressed_block)
	{
		if (offset_in_compressed_file == cur_begin_offset && offset_in_decompressed_block < working_buffer.size())
		{
			pos = working_buffer.begin() + offset_in_decompressed_block;
		}
		else
		{
			cur_end_offset = offset_in_compressed_file;
			pos = working_buffer.end();
			next();
			if (offset_in_decompressed_block > working_buffer.size())
				throw Exception("Seek position is beyond the decompressed block", ErrorCodes::ARGUMENT_OUT_OF_BOUND);
			pos += offset_in_decompressed_block;
		}
	}
};

}
