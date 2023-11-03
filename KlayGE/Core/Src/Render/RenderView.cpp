// RenderView.cpp
// KlayGE ��Ⱦ��ͼ�� ʵ���ļ�
// Ver 3.3.0
// ��Ȩ����(C) ������, 2006
// Homepage: http://www.klayge.org
//
// 3.3.0
// ���ν��� (2006.5.31)
//
// �޸ļ�¼
//////////////////////////////////////////////////////////////////////////////////

#include <KlayGE/KlayGE.hpp>
#include <KlayGE/RenderView.hpp>

namespace KlayGE
{
	ShaderResourceView::ShaderResourceView() = default;
	ShaderResourceView::~ShaderResourceView() noexcept = default;

	RenderTargetView::RenderTargetView() = default;
	RenderTargetView::~RenderTargetView() noexcept = default;

	DepthStencilView::DepthStencilView() = default;
	DepthStencilView::~DepthStencilView() noexcept = default;

	UnorderedAccessView::UnorderedAccessView() = default;
	UnorderedAccessView::~UnorderedAccessView() noexcept = default;
}
