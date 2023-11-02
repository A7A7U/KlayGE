// D3D11VideoMode.hpp
// KlayGE D3D11��ʾģʽ ͷ�ļ�
// Ver 3.8.0
// ��Ȩ����(C) ������, 2009
// Homepage: http://www.klayge.org
//
// 3.8.0
// ���ν��� (2008.9.21)
//
// �޸ļ�¼
/////////////////////////////////////////////////////////////////////////////////

#ifndef _D3D11VIDEOMODE_HPP
#define _D3D11VIDEOMODE_HPP

#pragma once

#include <directx/dxgiformat.h>

#include <KFL/Operators.hpp>

namespace KlayGE
{
	// ������ʾģʽ��Ϣ
	/////////////////////////////////////////////////////////////////////////////////
	class D3D11VideoMode final
	{
	public:
		D3D11VideoMode();
		D3D11VideoMode(uint32_t width, uint32_t height, DXGI_FORMAT format);

		uint32_t Width() const noexcept;
		uint32_t Height() const noexcept;
		DXGI_FORMAT Format() const noexcept;

		bool operator<(D3D11VideoMode const& rhs) const noexcept;
		bool operator==(D3D11VideoMode const& rhs) const noexcept;

		KLAYGE_DEFAULT_LESS_COMPARE_OPERATOR(D3D11VideoMode);
		KLAYGE_DEFAULT_EQUALITY_COMPARE_OPERATOR(D3D11VideoMode);

	private:
		uint32_t		width_;
		uint32_t		height_;
		DXGI_FORMAT		format_;
	};
}

#endif			// _D3D11VIDEOMODE_HPP
