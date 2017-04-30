/*
 *	Copyright (C) 2007-2009 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "GSDump.h"

#ifndef LZMA_SUPPORTED

GSDump::GSDump()
	: m_gs(NULL)
	, m_frames(0)
	, m_extra_frames(0)
{
}

GSDump::~GSDump()
{
	Close();
}

void GSDump::Open(const string& fn, uint32 crc, const GSFreezeData& fd, const GSPrivRegSet* regs)
{
	m_gs = fopen((fn + ".gs").c_str(), "wb");

	m_frames = 0;
	m_extra_frames = 2;

	if(m_gs)
	{
		fwrite(&crc, 4, 1, m_gs);
		fwrite(&fd.size, 4, 1, m_gs);
		fwrite(fd.data, fd.size, 1, m_gs);
		fwrite(regs, sizeof(*regs), 1, m_gs);
	}
}

void GSDump::Close()
{
	if(m_gs) {fclose(m_gs); m_gs = NULL;}
}

void GSDump::Transfer(int index, const uint8* mem, size_t size)
{
	if(m_gs && size > 0)
	{
		fputc(0, m_gs);
		fputc(index, m_gs);
		fwrite(&size, 4, 1, m_gs);
		fwrite(mem, size, 1, m_gs);
	}
}

void GSDump::ReadFIFO(uint32 size)
{
	if(m_gs && size > 0)
	{
		fputc(2, m_gs);
		fwrite(&size, 4, 1, m_gs);
	}
}

void GSDump::VSync(int field, bool last, const GSPrivRegSet* regs)
{
	if(m_gs)
	{
		fputc(3, m_gs);
		fwrite(regs, sizeof(*regs), 1, m_gs);

		fputc(1, m_gs);
		fputc(field, m_gs);

		if((++m_frames & 1) == 0 && last && (m_extra_frames <= 0))
		{
			Close();
		} else if (last) {
			m_extra_frames--;
		}
	}
}

#endif

#ifdef LZMA_SUPPORTED

GSDump::GSDump()
	: m_gs(nullptr)
	, m_frames(0)
	, m_extra_frames(0)
{
	m_in_buff.clear();
}

GSDump::~GSDump()
{
	Close();
}

void GSDump::Open(const string& fn, uint32 crc, const GSFreezeData& fd, const GSPrivRegSet* regs)
{
	Close();

	m_frames = 0;
	m_extra_frames = 2;

	m_strm = LZMA_STREAM_INIT;
	lzma_ret ret = lzma_easy_encoder(&m_strm, 6 /*level*/, LZMA_CHECK_CRC64);
	if (ret != LZMA_OK) {
		fprintf(stderr, "Error initializing LZMA encoder ! (error code %u)\n", ret);
		return;
	}

	m_gs = fopen((fn + ".gs.xz").c_str(), "wb");
	if (!m_gs)
		return;

	AppendRawData(&crc, 4);
	AppendRawData(&fd.size, 4);
	AppendRawData(fd.data, fd.size);
	AppendRawData(regs, sizeof(*regs));
}

void GSDump::Close()
{
	Compress(LZMA_FINISH);

	if (!m_gs)
		return;

	fclose(m_gs);
	m_gs = nullptr;
}

void GSDump::Compress(lzma_action action)
{
	if (!m_gs)
	{
		m_in_buff.clear(); // output file isn't open we can drop current data
		return;
	}

	if (m_in_buff.empty())
		return;

	lzma_action act = (action == LZMA_FINISH) ? LZMA_FINISH : LZMA_RUN;

	m_strm.next_in = &m_in_buff[0];
	m_strm.avail_in = m_in_buff.size();

	std::vector<uint8> out_buff(1024*1024);
	do {
		m_strm.next_out = &out_buff[0];
		m_strm.avail_out = out_buff.size();

		lzma_ret ret = lzma_code(&m_strm, act);

		if ((ret != LZMA_OK) && (ret != LZMA_STREAM_END)) {
			fprintf (stderr, "GSDump::Compress error: %d\n", (int) ret);
			m_in_buff.clear();
			Close();
		}

		size_t write_size = out_buff.size() - m_strm.avail_out;
		fwrite(&out_buff[0], write_size, 1, m_gs);

	} while (m_strm.avail_out == 0);

	m_in_buff.clear();

	if (action == LZMA_FINISH)
		lzma_end(&m_strm);
}

void GSDump::AppendRawData(const void *data, size_t size)
{
	size_t old_size = m_in_buff.size();
	m_in_buff.resize(old_size + size);
	memcpy(&m_in_buff[old_size], data, size);

	// Enough data was accumulated, time to compress it.
	// It will freeze PCSX2. 200MB should be enough for long dump.
	if (m_in_buff.size() > 200*1024*1024)
		Compress(LZMA_RUN);
}

void GSDump::AppendRawData(uint8 c)
{
	m_in_buff.push_back(c);
}

void GSDump::Transfer(int index, const uint8* mem, size_t size)
{
	if (size == 0)
		return;

	AppendRawData(0);
	AppendRawData(index);
	AppendRawData(&size, 4);
	AppendRawData(mem, size);
}

void GSDump::ReadFIFO(uint32 size)
{
	if (size == 0)
		return;

	AppendRawData(2);
	AppendRawData(&size, 4);
}

void GSDump::VSync(int field, bool last, const GSPrivRegSet* regs)
{
	AppendRawData(3);
	AppendRawData(regs, sizeof(*regs));

	AppendRawData(1);
	AppendRawData(field);

	if((++m_frames & 1) == 0 && last && (m_extra_frames <= 0))
		Close();
	else if (last)
		m_extra_frames--;
}

#endif
