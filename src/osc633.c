#include "osc633.h"
#include <string.h>

/* 统一文本协议的 OSC 扫描：ESC ] <body> (ST=ESC \\ | BEL)。
 * 逻辑与客户端历史实现一致，但以回调形式与角色/相位策略解耦。 */

#define OSC_ESC 0x1b
#define OSC_BEL 0x07

void
osc633_scan_init(osc633_scan_t *s)
{
	s->hold_len = 0;
}

/* 交付一个判定完整的 OSC 序列：区分 633 与其它 OSC。 */
static void
deliver_mark(const uint8_t *seq, size_t seqlen,
             size_t body_len, void *ctx,
             osc633_mark_fn on_mark)
{
	const uint8_t *body = seq + 2;
	int is633 = (body_len > 4 && body[0] == '6' && body[1] == '3'
	             && body[2] == '3' && body[3] == ';');
	if (!is633) {
		on_mark(ctx, 0, 0, NULL, 0, seq, seqlen);
		return;
	}
	const uint8_t *q = body + 4;
	size_t qn = body_len - 4;
	char code = (char)q[0];
	const uint8_t *args = q + 1;
	size_t alen = qn - 1;
	if (alen && args[0] == ';') {
		args++;
		alen--;
	}
	on_mark(ctx, 1, code, args, alen, seq, seqlen);
}

void
osc633_scan_feed(osc633_scan_t *s, const uint8_t *in, size_t inlen,
                 void *ctx, osc633_raw_fn on_raw, osc633_mark_fn on_mark)
{
	uint8_t stackbuf[OSC633_HOLD_MAX + 65536];
	const uint8_t *p;
	size_t n;

	if (s->hold_len) {
		if (s->hold_len + inlen > sizeof stackbuf) {
			/* 理论不可达（hold < 1024，单块最大 65536）：保险先冲滞留 */
			on_raw(ctx, s->hold, s->hold_len);
			s->hold_len = 0;
		} else {
			memcpy(stackbuf, s->hold, s->hold_len);
			memcpy(stackbuf + s->hold_len, in, inlen);
			p = stackbuf;
			n = s->hold_len + inlen;
			s->hold_len = 0;
			goto scan;
		}
	}
	p = in;
	n = inlen;
scan: ;
	size_t i = 0;
	while (i < n) {
		if (p[i] != OSC_ESC) {
			/* 连续可见字节合并成一次回调，减少调用频次 */
			size_t b = i;
			while (i < n && p[i] != OSC_ESC)
				i++;
			on_raw(ctx, p + b, i - b);
			continue;
		}
		if (i + 1 >= n) {
			/* 缓冲末尾的孤立 ESC：滞留观察 */
			if (n - i < OSC633_HOLD_MAX) {
				memcpy(s->hold, p + i, n - i);
				s->hold_len = n - i;
			} else {
				on_raw(ctx, p + i, n - i);
			}
			return;
		}
		if (p[i + 1] != ']') {
			on_raw(ctx, p + i, 1); /* CSI 等其它 ESC 序列，原样放行 */
			i++;
			continue;
		}
		/* OSC：扫描终止符 ST(ESC \) 或 BEL */
		size_t j = i + 2;
		for (;;) {
			while (j < n && p[j] != OSC_ESC && p[j] != OSC_BEL)
				j++;
			if (j >= n)
				break; /* 缓冲结束，未见终止符 */
			if (p[j] == OSC_BEL) {
				deliver_mark(p + i, (size_t)(j + 1 - i),
				             (size_t)(j - (i + 2)),
				             ctx, on_mark);
				i = j + 1;
				goto next;
			}
			if (j + 1 >= n)
				break; /* 末尾 ESC：可能是 ST 半截，滞留 */
			if (p[j + 1] == '\\') {
				deliver_mark(p + i, (size_t)(j + 2 - i),
				             (size_t)(j - (i + 2)),
				             ctx, on_mark);
				i = j + 2;
				goto next;
			}
			j++; /* ESC 后跟别的：继续找终止符 */
		}
		/* 未见终止符：滞留整个候选序列（超长则放弃剥离，原样放行） */
		if (n - i < OSC633_HOLD_MAX) {
			memcpy(s->hold, p + i, n - i);
			s->hold_len = n - i;
		} else {
			on_raw(ctx, p + i, n - i);
		}
		return;
	next:
		;
	}
}
