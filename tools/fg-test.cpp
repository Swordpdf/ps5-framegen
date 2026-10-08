// RPCS3-PS5 (m73; ps5-framegen): runs the frame generation (src/ps5_framegen.cpp) on a host Vulkan device (SwiftShader) between two
// pictures, and writes the frame it makes between them.
//
//   fg-test A.png B.png OUT.png [--shift N]   with --shift, B is A moved N pixels right (A.png's B is ignored), and the result is
//                                              compared with A moved N/2 pixels (and with the plain blend of A and B)
//   fg-test --object OUT.png                    a synthetic test: a textured square moving 40 pixels over a textured background
//   fg-test --seq OUT.png [--debug 1] [--gen N] F1.png F2.png ...
//                                              m73f: real consecutive frames in order (a frame dump's game frames); writes the frame
//                                              between the last two
//   fg-test --scene NAME OUT.png [--bg BG.png] [--pan N] [--obj N] [--head-move N] [--debug 1] [--gen N]
//                                              m73b: a scene with a true middle frame, measured by region. The background (BG.png,
//                                              1280x720, e.g. a frame of the game; noise without it) pans N pixels a frame (12) under
//                                              what the scene adds, which stays still on screen like a game's HUD or the character
//                                              the camera follows:
//                                                pan    the background alone
//                                                head   a head and a body in the middle (still)
//                                                hud    HUD text, a minimap and a button prompt (still; cut from BG.png when given)
//                                                all    both, and a square moving --obj pixels a frame (20) on its own
//                                              OUT.png.cmp.png shows, around the head, the HUD and the square: A, B, the true middle
//                                              frame, the plain blend, the result and its error (x4). --debug 1: the result shows
//                                              the interpolation's decisions instead (ps5_framegen.h, set_debug_view).
//
// Build: tools/build-fg-test.sh. Run: VK_ICD_FILENAMES=<SwiftShader's vk_swiftshader_icd.json> fg-test ...
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_framegen.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
	struct picture
	{
		int w = 0, h = 0;
		std::vector<unsigned char> rgba;

		unsigned char* at(int x, int y) { return &rgba[(size_t(y) * w + x) * 4]; }
		const unsigned char* at(int x, int y) const { return &rgba[(size_t(y) * w + x) * 4]; }
	};

	bool load(const char* path, picture& p)
	{
		int n = 0;
		unsigned char* data = stbi_load(path, &p.w, &p.h, &n, 4);
		if (!data) return false;
		p.rgba.assign(data, data + size_t(p.w) * p.h * 4);
		stbi_image_free(data);
		return true;
	}

	void save(const std::string& path, const picture& p)
	{
		stbi_write_png(path.c_str(), p.w, p.h, 4, p.rgba.data(), p.w * 4);
	}

	picture shifted(const picture& a, int dx)
	{
		picture b = a;
		for (int y = 0; y < a.h; y++)
			for (int x = 0; x < a.w; x++)
			{
				const int sx = std::min(std::max(x - dx, 0), a.w - 1);
				std::memcpy(b.at(x, y), a.at(sx, y), 4);
			}
		return b;
	}

	double diff(const picture& a, const picture& b, int margin)
	{
		double sum = 0;
		size_t n = 0;
		for (int y = margin; y < a.h - margin; y++)
			for (int x = margin; x < a.w - margin; x++)
				for (int c = 0; c < 3; c++)
				{
					sum += std::abs(int(a.at(x, y)[c]) - int(b.at(x, y)[c]));
					n++;
				}
		return sum / n;
	}

	picture blend(const picture& a, const picture& b)
	{
		picture c = a;
		for (size_t i = 0; i < a.rgba.size(); i++) c.rgba[i] = static_cast<unsigned char>((a.rgba[i] + b.rgba[i] + 1) / 2);
		return c;
	}

	// A noise texture, smoothed a little so the flow has texture without aliasing.
	picture noise_picture(int w, int h, unsigned seed)
	{
		picture p;
		p.w = w; p.h = h; p.rgba.assign(size_t(w) * h * 4, 255);
		std::vector<unsigned char> bg(size_t(w) * h * 3);
		for (auto& v : bg) { seed = seed * 1103515245 + 12345; v = static_cast<unsigned char>(64 + ((seed >> 16) & 63)); }
		for (int pass = 0; pass < 2; pass++)
			for (int y = 1; y < h - 1; y++)
				for (int x = 1; x < w - 1; x++)
					for (int c = 0; c < 3; c++)
					{
						int s = 0;
						for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) s += bg[(size_t(y + j) * w + x + i) * 3 + c];
						bg[(size_t(y) * w + x) * 3 + c] = static_cast<unsigned char>(s / 9);
					}
		for (int i = 0; i < w * h; i++) { p.rgba[i * 4] = bg[i * 3]; p.rgba[i * 4 + 1] = bg[i * 3 + 1]; p.rgba[i * 4 + 2] = bg[i * 3 + 2]; }
		return p;
	}

	// A synthetic frame: a noise-textured background and a checker square at x = ox.
	void make_object(picture& a, picture& b, picture& mid, int dx, int base = 200)
	{
		const int w = 640, h = 360;
		const picture bg = noise_picture(w, h, 12345);
		for (picture* p : {&a, &b, &mid}) *p = bg;
		const auto draw = [&](picture& p, int ox)
		{
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
				{
					unsigned char* px = p.at(x, y);
					const int lx = x - ox, ly = y - 120;
					if (lx >= 0 && lx < 96 && ly >= 0 && ly < 96)
					{
						const bool on = ((lx / 12) + (ly / 12)) & 1;
						px[0] = on ? 230 : 30; px[1] = on ? 200 : 60; px[2] = on ? 40 : 200;
					}
				}
		};
		draw(a, base);
		draw(b, base + dx);
		draw(mid, base + dx / 2);
	}

	// m73b: the scenes. What is drawn at each pixel (id): 0 the background, 1 the head or the body, 2 the HUD, 3 the moving square.
	struct scene_t
	{
		int w = 960, h = 540;
		picture bg;      // at least (w + 320) x h: the window moves over it
		int pan = 12;    // background pixels a frame (the content moves left)
		bool head = false, hud = false;
		int obj = 0;     // the square's pixels a frame (right)
		int head_move = 0; // the head and body's pixels a frame (right; 0: still, the character the camera follows exactly)
		picture hud_layer; // w x h, alpha = coverage
	};

	unsigned char clamp8(int v) { return static_cast<unsigned char>(std::min(255, std::max(0, v))); }

	void build_hud(scene_t& s, const picture* frame)
	{
		picture& l = s.hud_layer;
		l.w = s.w; l.h = s.h; l.rgba.assign(size_t(s.w) * s.h * 4, 0);
		const auto put = [&](int x, int y, int r, int g, int b)
		{
			if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
			unsigned char* p = l.at(x, y);
			p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b); p[3] = 255;
		};

		if (frame && frame->w >= 1280 && frame->h >= 720)
		{
			// inFamous's "Point of Interest" (orange text with a dark edge) at (540..740, 500..535) of the 1280x720 frame: the text
			// pixels and those next to them
			std::vector<unsigned char> key(200 * 36, 0);
			for (int y = 0; y < 36; y++)
				for (int x = 0; x < 200; x++)
				{
					const unsigned char* p = frame->at(540 + x, 500 + y);
					key[y * 200 + x] = p[0] > 170 && p[1] > 100 && p[2] < 110 && p[0] > p[2] + 80;
				}
			for (int y = 0; y < 36; y++)
				for (int x = 0; x < 200; x++)
				{
					bool near = false;
					for (int j = -1; j <= 1 && !near; j++)
						for (int i = -1; i <= 1 && !near; i++)
						{
							const int xx = x + i, yy = y + j;
							near = xx >= 0 && yy >= 0 && xx < 200 && yy < 36 && key[yy * 200 + xx];
						}
					if (near)
					{
						const unsigned char* p = frame->at(540 + x, 500 + y);
						put(380 + x, 360 + y, p[0], p[1], p[2]);
					}
				}
			// the button prompt below it (a circle of radius 20 around (640, 556)) and the minimap (radius 62 around (1122, 603))
			const auto disc = [&](int cx, int cy, int r, int ox, int oy)
			{
				for (int y = -r; y <= r; y++)
					for (int x = -r; x <= r; x++)
						if (x * x + y * y <= r * r)
						{
							const unsigned char* p = frame->at(cx + x, cy + y);
							put(ox + x, oy + y, p[0], p[1], p[2]);
						}
			};
			disc(640, 556, 20, 480, 418);
			disc(1122, 603, 62, s.w - 80, s.h - 80);
		}
		else
		{
			// a minimap: a dark disc with a light ring and a few roads
			for (int y = -62; y <= 62; y++)
				for (int x = -62; x <= 62; x++)
				{
					const int d2 = x * x + y * y;
					if (d2 > 62 * 62) continue;
					int r = 40, g = 44, b = 52;
					if (d2 > 56 * 56) { r = 20; g = 30; b = 70; }
					if (std::abs(x - y / 3) < 3 || std::abs(y + 10) < 2) { r = 150; g = 150; b = 140; }
					put(s.w - 80 + x, s.h - 80 + y, r, g, b);
				}
		}

		// a subtitle: white strokes (3 pixels) with a dark edge, 14 letters of 16x24
		for (int i = 0; i < 14; i++)
		{
			const int ox = 300 + i * 22, oy = s.h - 60;
			const int kind = (i * 7 + 3) % 5;
			for (int y = -1; y < 25; y++)
				for (int x = -1; x < 17; x++)
				{
					const auto stroke = [&](int xx, int yy)
					{
						if (xx < 0 || yy < 0 || xx >= 16 || yy >= 24) return false;
						switch (kind)
						{
						case 0: return xx < 3 || xx >= 13 || yy < 3;                 // n
						case 1: return xx < 3 || yy < 3 || yy >= 21 || (yy >= 10 && yy < 13); // E
						case 2: return xx < 3 || yy >= 21;                           // L
						case 3: return std::abs(xx - (yy * 16 / 24)) < 2 || std::abs(15 - xx - (yy * 16 / 24)) < 2; // X
						default: return (xx >= 6 && xx < 9) || yy < 3;               // T
						}
					};
					if (stroke(x, y)) put(ox + x, oy + y, 245, 245, 240);
					else
					{
						bool edge = false;
						for (int j = -1; j <= 1 && !edge; j++)
							for (int k = -1; k <= 1 && !edge; k++)
								edge = stroke(x + k, y + j);
						if (edge) put(ox + x, oy + y, 10, 10, 10);
					}
				}
		}

		// a health bar at the top left
		for (int y = 30; y < 44; y++)
			for (int x = 30; x < 230; x++)
			{
				const bool edge = y < 32 || y >= 42 || x < 32 || x >= 228;
				if (edge) put(x, y, 0, 0, 0);
				else if (x < 170) put(x, y, 200, 30 + (x - 32) / 2, 40);
				else put(x, y, 60, 20, 20);
			}
	}

	// The scene at time t (in frames; a half for the true middle frame), and what each pixel shows (ids).
	picture render(const scene_t& s, double t, std::vector<unsigned char>* ids)
	{
		picture p;
		p.w = s.w; p.h = s.h; p.rgba.assign(size_t(s.w) * s.h * 4, 255);
		if (ids) ids->assign(size_t(s.w) * s.h, 0);
		const int ox = 40 + static_cast<int>(std::lround(s.pan * t));
		for (int y = 0; y < s.h; y++)
			for (int x = 0; x < s.w; x++)
			{
				const int sx = std::min(std::max(x + ox, 0), s.bg.w - 1);
				std::memcpy(p.at(x, y), s.bg.at(sx, std::min(y, s.bg.h - 1)), 4);
				p.at(x, y)[3] = 255;
			}

		const auto mark = [&](int x, int y, unsigned char id) { if (ids) (*ids)[size_t(y) * s.w + x] = id; };

		if (s.obj)
		{
			const int qx = 100 + static_cast<int>(std::lround(s.obj * t)), qy = 120;
			for (int y = 0; y < 72; y++)
				for (int x = 0; x < 72; x++)
				{
					const int px = qx + x, py = qy + y;
					if (px < 0 || px >= s.w || py >= s.h) continue;
					const bool on = ((x / 9) + (y / 9)) & 1;
					unsigned char* d = p.at(px, py);
					d[0] = on ? 230 : 30; d[1] = on ? 200 : 60; d[2] = on ? 40 : 200;
					mark(px, py, 3);
				}
		}

		if (s.head)
		{
			// a head (an ellipse, shaded skin with an ear and a few marks) over a body (a jacket with seams): still on screen
			const int cx = s.w / 2 + static_cast<int>(std::lround(s.head_move * t)) - s.head_move * 6, cy = s.h / 2 - 60;
			for (int y = cy + 34; y < s.h; y++)
				for (int x = cx - 64; x <= cx + 64; x++)
				{
					const int dy = y - (cy + 34);
					const int half = std::min(64, 30 + dy);
					if (std::abs(x - cx) > half) continue;
					unsigned char* d = p.at(x, y);
					const bool edge = std::abs(x - cx) > half - 2;
					const int seam = ((x - cx + 64) % 23 < 2) || (dy % 31 < 2);
					const int tex = (((x - cx + 640) * 7 + y * 13) % 11) * 3; // moves with the body
					d[0] = clamp8(edge ? 20 : 96 - seam * 40 + tex); d[1] = clamp8(edge ? 20 : 92 - seam * 40 + tex); d[2] = clamp8(edge ? 20 : 60 - seam * 30);
					mark(x, y, 1);
				}
			for (int y = cy - 46; y <= cy + 46; y++)
				for (int x = cx - 38; x <= cx + 38; x++)
				{
					const double ex = (x - cx) / 36.0, ey = (y - cy) / 44.0;
					const double r = ex * ex + ey * ey;
					if (r > 1.0) continue;
					unsigned char* d = p.at(x, y);
					if (r > 0.88) { d[0] = 40; d[1] = 25; d[2] = 20; }
					else
					{
						const double shade = 0.65 + 0.35 * (1.0 - r) + 0.15 * (-(x - cx) / 36.0);
						int rr = static_cast<int>(205 * shade), gg = static_cast<int>(150 * shade), bb = static_cast<int>(120 * shade);
						if (std::abs(x - (cx + 22)) < 5 && std::abs(y - cy) < 9) { rr -= 60; gg -= 50; bb -= 40; }   // ear
						if (std::abs(y - (cy + 20)) < 2 && std::abs(x - cx) < 14) { rr -= 80; gg -= 70; bb -= 60; }  // a mark
						d[0] = clamp8(rr); d[1] = clamp8(gg); d[2] = clamp8(bb);
					}
					mark(x, y, 1);
				}
		}

		if (s.hud)
		{
			for (int y = 0; y < s.h; y++)
				for (int x = 0; x < s.w; x++)
				{
					const unsigned char* l = s.hud_layer.at(x, y);
					if (!l[3]) continue;
					std::memcpy(p.at(x, y), l, 3);
					mark(x, y, 2);
				}
		}
		return p;
	}

	// Pixels within r of id (dilate) as a mask.
	std::vector<unsigned char> dilate(const std::vector<unsigned char>& m, int w, int h, int r)
	{
		std::vector<unsigned char> t(m.size(), 0), o(m.size(), 0);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				unsigned char v = 0;
				for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r) && !v; i++) v = m[size_t(y) * w + i];
				t[size_t(y) * w + x] = v;
			}
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				unsigned char v = 0;
				for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r) && !v; j++) v = t[size_t(j) * w + x];
				o[size_t(y) * w + x] = v;
			}
		return o;
	}

	// The regions measured: 1 around the head's edge, 2 the HUD, 3 around the square's edge, 0 the rest (24 pixels from the
	// picture's edges are not measured: 255).
	std::vector<unsigned char> regions(const std::vector<unsigned char>& ids, int w, int h)
	{
		std::vector<unsigned char> reg(ids.size(), 0);
		const auto ring = [&](unsigned char id, int r, bool area)
		{
			std::vector<unsigned char> in(ids.size()), out(ids.size());
			for (size_t i = 0; i < ids.size(); i++) { in[i] = ids[i] == id; out[i] = ids[i] != id; }
			const auto di = dilate(in, w, h, r);
			const auto dout = dilate(out, w, h, r);
			for (size_t i = 0; i < ids.size(); i++)
				if (di[i] && (area || dout[i])) reg[i] = id;
		};
		ring(3, 12, false);
		ring(1, 12, false);
		ring(2, 6, true);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (x < 24 || y < 24 || x >= w - 24 || y >= h - 24) reg[size_t(y) * w + x] = 255;
		return reg;
	}

	void region_errors(const picture& a, const picture& b, const std::vector<unsigned char>& reg, double out[4], int count[4])
	{
		double sum[4] = {};
		for (int i = 0; i < 4; i++) count[i] = 0;
		for (size_t i = 0; i < reg.size(); i++)
		{
			if (reg[i] > 3) continue;
			int d = 0;
			for (int c = 0; c < 3; c++) d += std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]));
			sum[reg[i]] += d / 3.0;
			count[reg[i]]++;
		}
		for (int i = 0; i < 4; i++) out[i] = count[i] ? sum[i] / count[i] : 0.0;
	}

	// Crops of A, B, the truth, the blend, the result and its error x4, side by side, one row per box.
	picture comparison(const std::vector<const picture*>& pics, const std::vector<std::array<int, 4>>& boxes)
	{
		int cw = 0, ch = 0;
		for (const auto& b : boxes) { cw = std::max(cw, b[2]); ch += b[3] + 4; }
		picture c;
		c.w = (cw + 4) * int(pics.size() + 1); c.h = ch; c.rgba.assign(size_t(c.w) * c.h * 4, 255);
		int oy = 0;
		for (const auto& b : boxes)
		{
			for (size_t k = 0; k <= pics.size(); k++)
				for (int y = 0; y < b[3]; y++)
					for (int x = 0; x < b[2]; x++)
					{
						unsigned char* d = c.at(int(k) * (cw + 4) + x, oy + y);
						if (k < pics.size())
						{
							std::memcpy(d, pics[k]->at(b[0] + x, b[1] + y), 3);
						}
						else
						{
							// the result (pics[4]) against the truth (pics[2])
							for (int ch2 = 0; ch2 < 3; ch2++)
								d[ch2] = clamp8(4 * std::abs(int(pics[4]->at(b[0] + x, b[1] + y)[ch2]) - int(pics[2]->at(b[0] + x, b[1] + y)[ch2])));
						}
						d[3] = 255;
					}
			oy += b[3] + 4;
		}
		return c;
	}

	[[noreturn]] void die(const char* what, VkResult r = VK_SUCCESS)
	{
		std::fprintf(stderr, "fg-test: %s (%d)\n", what, int(r));
		std::exit(1);
	}

	struct gpu
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = 0;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkPhysicalDeviceMemoryProperties mem{};

		void init()
		{
			VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
			app.apiVersion = VK_API_VERSION_1_1;
			VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
			ici.pApplicationInfo = &app;
			if (VkResult r = vkCreateInstance(&ici, nullptr, &instance)) die("vkCreateInstance", r);
			uint32_t n = 1;
			if (VkResult r = vkEnumeratePhysicalDevices(instance, &n, &physical); r < 0 || !n) die("no Vulkan device", r);
			VkPhysicalDeviceProperties props{};
			vkGetPhysicalDeviceProperties(physical, &props);
			std::printf("device: %s\n", props.deviceName);
			vkGetPhysicalDeviceMemoryProperties(physical, &mem);
			uint32_t qn = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
			std::vector<VkQueueFamilyProperties> qf(qn);
			vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qf.data());
			for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
			const float prio = 1.f;
			VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			qci.queueFamilyIndex = family;
			qci.queueCount = 1;
			qci.pQueuePriorities = &prio;
			VkPhysicalDeviceFeatures features{};
			features.shaderStorageImageExtendedFormats = VK_TRUE;
			VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			dci.queueCreateInfoCount = 1;
			dci.pQueueCreateInfos = &qci;
			dci.pEnabledFeatures = &features;
			if (VkResult r = vkCreateDevice(physical, &dci, nullptr, &device)) die("vkCreateDevice", r);
			vkGetDeviceQueue(device, family, 0, &queue);
			VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pci.queueFamilyIndex = family;
			if (VkResult r = vkCreateCommandPool(device, &pci, nullptr, &pool)) die("vkCreateCommandPool", r);
		}

		uint32_t type(uint32_t bits, VkMemoryPropertyFlags f)
		{
			for (uint32_t i = 0; i < mem.memoryTypeCount; i++) if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & f) == f) return i;
			die("memory type");
		}

		void buffer(VkDeviceSize size, VkBuffer& b, VkDeviceMemory& m, void** map)
		{
			VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bci.size = size;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			if (vkCreateBuffer(device, &bci, nullptr, &b)) die("vkCreateBuffer");
			VkMemoryRequirements req{};
			vkGetBufferMemoryRequirements(device, b, &req);
			VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			ai.allocationSize = req.size;
			ai.memoryTypeIndex = type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (vkAllocateMemory(device, &ai, nullptr, &m)) die("vkAllocateMemory");
			vkBindBufferMemory(device, b, m, 0);
			vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, map);
		}

		VkCommandBuffer begin()
		{
			VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			ai.commandPool = pool;
			ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			ai.commandBufferCount = 1;
			VkCommandBuffer cmd;
			vkAllocateCommandBuffers(device, &ai, &cmd);
			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkBeginCommandBuffer(cmd, &bi);
			return cmd;
		}

		void submit(VkCommandBuffer cmd)
		{
			vkEndCommandBuffer(cmd);
			VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			if (VkResult r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)) die("vkQueueSubmit", r);
			if (VkResult r = vkQueueWaitIdle(queue)) die("vkQueueWaitIdle", r);
			vkFreeCommandBuffers(device, pool, 1, &cmd);
		}
	};

	void upload(VkCommandBuffer cmd, VkBuffer staging, VkImage image, uint32_t w, uint32_t h)
	{
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
	}

	void download(VkCommandBuffer cmd, VkImage image, VkBuffer staging, uint32_t w, uint32_t h)
	{
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, staging, 1, &region);
	}
}

int main(int argc, char** argv)
{
	picture a, b, truth;
	const char* out = nullptr;
	bool have_truth = false;

	// The frames: frame(k) is the picture of frame k; the last pair (frames - 2, frames - 1) is measured against `truth`.
	// 14 frames for the scenes: the interpolation's still history needs 8 frames in a row (FG_STILL_FRAMES); --seq: as many as given
	int seq_frames = argc - 3;
	for (int i = 3; i + 1 < argc; i++)
		if (std::strcmp(argv[i], "--debug") == 0 || std::strcmp(argv[i], "--gen") == 0) seq_frames -= 2;
	const int frames = (argc >= 2 && std::strcmp(argv[1], "--scene") == 0) ? 14 : (argc >= 2 && std::strcmp(argv[1], "--seq") == 0) ? std::max(2, seq_frames) : 8;
	static int frames_static;
	frames_static = frames;
	std::function<picture(int)> frame_at;
	std::vector<unsigned char> reg; // m73b scenes: the regions measured
	std::vector<std::array<int, 4>> boxes;
	const bool object = argc >= 3 && std::strcmp(argv[1], "--object") == 0;
	bool debug_view = false; // --debug 1: the last interpolated frame shows the decisions (set_debug_view)
	int gen = 1;             // m74 --gen N: N frames generated between the last two (interpolator::set_generated), OUT.png, OUT.1.png, ...
	static scene_t* scene = nullptr;

	if (argc >= 4 && std::strcmp(argv[1], "--scene") == 0)
	{
		static scene_t s;
		const std::string name = argv[2];
		out = argv[3];
		const char* bgpath = nullptr;
		for (int i = 4; i + 1 < argc; i += 2)
		{
			if (std::strcmp(argv[i], "--bg") == 0) bgpath = argv[i + 1];
			else if (std::strcmp(argv[i], "--pan") == 0) s.pan = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--obj") == 0) s.obj = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--debug") == 0) debug_view = std::atoi(argv[i + 1]) != 0;
			else if (std::strcmp(argv[i], "--head-move") == 0) s.head_move = std::atoi(argv[i + 1]);
			else if (std::strcmp(argv[i], "--gen") == 0) gen = std::max(1, std::min(3, std::atoi(argv[i + 1])));
			else die("bad option");
		}
		if (s.pan & 1) die("--pan must be even (the middle frame is half of it)");
		picture frame;
		if (bgpath && !load(bgpath, frame)) die("cannot load --bg");
		if (bgpath) s.bg = frame; else s.bg = noise_picture(s.w + 320, s.h, 777);
		if (s.bg.w < s.w + 320 || s.bg.h < s.h) die("--bg too small (1280x720 or more)");
		s.head = name == "head" || name == "all";
		s.hud = name == "hud" || name == "all";
		if (name == "all" && !s.obj) s.obj = 20;
		if (name != "all") s.obj = 0;
		if (name != "pan" && !s.head && !s.hud) die("scene: pan, head, hud or all");
		build_hud(s, bgpath ? &frame : nullptr);
		std::printf("scene %s: %dx%d, background %s panning %d px a frame, square %d px a frame\n", name.c_str(), s.w, s.h, bgpath ? bgpath : "noise", s.pan, s.obj);
		frame_at = [](int k) { return render(s, k, nullptr); };
		std::vector<unsigned char> ids;
		scene = &s;
		truth = render(s, frames - 2 + 1.0 / (gen + 1), &ids); // m74: the first frame generated, a 1 / (gen + 1) of the way
		reg = regions(ids, s.w, s.h);
		a = render(s, frames - 2, nullptr);
		b = render(s, frames - 1, nullptr);
		have_truth = true;
		boxes.push_back({s.w / 2 - 110, s.h / 2 - 130, 220, 200});            // the head
		boxes.push_back({360, 340, 240, 110});                               // the HUD text and prompt
		boxes.push_back({290, s.h - 70, 330, 40});                           // the subtitle
		if (s.obj) boxes.push_back({100 + int(s.obj * (frames - 1.5)) - 30, 100, 140, 110}); // the square
	}
	else if (argc >= 5 && std::strcmp(argv[1], "--seq") == 0)
	{
		// m73f: real consecutive frames (a frame dump's game frames, in order): fed one after another, and the frame between the last
		// two written (no truth to measure against).
		out = argv[2];
		static std::vector<picture> seq;
		for (int i = 3; i < argc; i++)
		{
			if (std::strcmp(argv[i], "--debug") == 0 && i + 1 < argc)
			{
				debug_view = std::atoi(argv[++i]) != 0;
				continue;
			}
			if (std::strcmp(argv[i], "--gen") == 0 && i + 1 < argc)
			{
				gen = std::max(1, std::min(3, std::atoi(argv[++i])));
				continue;
			}
			picture p;
			if (!load(argv[i], p)) die("cannot load a --seq frame");
			if (!seq.empty() && (p.w != seq[0].w || p.h != seq[0].h)) die("--seq frames of another size");
			seq.push_back(std::move(p));
		}
		seq_frames = static_cast<int>(seq.size());
		frame_at = [](int k) { return seq[static_cast<size_t>(k)]; };
		a = seq[seq.size() - 2];
		b = seq.back();
	}
	else if (object)
	{
		out = argv[2];
		frame_at = [](int k)
		{
			picture p0, p1, mid;
			make_object(p0, p1, mid, 40, 40 * k); // the square at 40 + 40k
			return p1;
		};
		make_object(a, b, truth, 40, 40 * (frames - 1));
		have_truth = true;
		save(std::string(out) + ".A.png", a);
		save(std::string(out) + ".B.png", b);
	}
	else if (argc >= 4)
	{
		if (!load(argv[1], a)) die("cannot load A");
		out = argv[3];
		if (argc >= 6 && std::strcmp(argv[4], "--shift") == 0)
		{
			static picture base;
			static int step;
			base = a;
			step = std::atoi(argv[5]);
			frame_at = [](int k) { return shifted(base, step * k); };
			truth = shifted(base, step * (frames - 1) - step / 2);
			a = shifted(base, step * (frames - 2));
			b = shifted(base, step * (frames - 1));
			have_truth = true;
		}
		else
		{
			if (!load(argv[2], b) || b.w != a.w || b.h != a.h) die("cannot load B (or another size)");
			static picture pa, pb;
			pa = a; pb = b;
			frame_at = [](int k) { return ((frames_static - 1 - k) & 1) ? pa : pb; };
		}
	}
	else
	{
		std::fprintf(stderr, "usage: fg-test A.png B.png OUT.png [--shift N] | fg-test --object OUT.png | fg-test --scene pan|head|hud|all OUT.png [--bg BG.png] [--pan N] [--obj N]\n");
		return 2;
	}

	gpu g;
	g.init();

	ps5::framegen::interpolator fg;
	std::string error;
	const auto t0 = std::chrono::steady_clock::now();
	// FG_TEST_BGRA=1: the frames in B8G8R8A8_UNORM (create()'s frame_format, as a renderer drawing in a BGRA swapchain's format
	// has them), uploaded with R and B swapped; the result must be the same as with R8G8B8A8.
	const char* bgra_env = std::getenv("FG_TEST_BGRA");
	const bool bgra = bgra_env && bgra_env[0] == '1';
	if (!fg.create(g.physical, g.device, a.w, a.h, error, bgra ? VK_FORMAT_B8G8R8A8_UNORM : ps5::framegen::interpolator::k_format))
		die(("create: " + error).c_str());
	if (bgra) std::printf("frames in B8G8R8A8_UNORM\n");
	const auto t1 = std::chrono::steady_clock::now();
	std::printf("created for %dx%d in %.0f ms\n", a.w, a.h, std::chrono::duration<double, std::milli>(t1 - t0).count());

	const size_t bytes = size_t(a.w) * a.h * 4;
	VkBuffer staging; VkDeviceMemory staging_mem; void* map = nullptr;
	g.buffer(bytes * 2 + 4096, staging, staging_mem, &map);

	// A reset, then frames moving on: FSR3 does not interpolate for a few frames after a reset or a scene change (its scene change
	// detection keeps 4 frames of history), so the last pair is the one measured.
	fg.set_generated(static_cast<uint32_t>(gen));

	for (int frame = 0; frame < frames; frame++)
	{
		const picture cur = frame_at(frame);
		std::memcpy(map, cur.rgba.data(), bytes);
		if (bgra)
		{
			unsigned char* px = static_cast<unsigned char*>(map);
			for (size_t i = 0; i < bytes; i += 4) std::swap(px[i], px[i + 2]);
		}
		VkCommandBuffer cmd = g.begin();
		fg.prepare(cmd);
		fg.set_debug_view(debug_view && frame == frames - 1);
		upload(cmd, staging, fg.frame_image(), a.w, a.h);
		const bool made = fg.record(cmd, frame == 0);
		if (made && frame == frames - 1) download(cmd, fg.output_image(), staging, a.w, a.h);
		// the flow after the output in the staging buffer
		const uint32_t fw = fg.flow_width(), fh = fg.flow_height();
		const size_t flow_at = bytes;
		{
			VkBufferImageCopy r{};
			r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			r.imageExtent = {fw, fh, 1};
			r.bufferOffset = flow_at;
			vkCmdCopyImageToBuffer(cmd, fg.debug_flow(), VK_IMAGE_LAYOUT_GENERAL, staging, 1, &r);
			r.bufferOffset = flow_at + size_t(fw) * fh * 4;
			vkCmdCopyImageToBuffer(cmd, fg.debug_field_x(), VK_IMAGE_LAYOUT_GENERAL, staging, 1, &r);
		}
		const auto s0 = std::chrono::steady_clock::now();
		g.submit(cmd);
		const auto s1 = std::chrono::steady_clock::now();
		{
			const int16_t* flow = reinterpret_cast<const int16_t*>(static_cast<const unsigned char*>(map) + flow_at);
			const uint32_t* fx = reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(map) + flow_at + size_t(fw) * fh * 4);
			double sx = 0, sy = 0; int nz = 0, fieldnz = 0;
			for (uint32_t i = 0; i < fw * fh; i++)
			{
				sx += flow[i * 2]; sy += flow[i * 2 + 1];
				if (flow[i * 2] || flow[i * 2 + 1]) nz++;
				if (fx[i]) fieldnz++;
			}
			std::printf("frame %d: %s, %.0f ms on this device; flow %ux%u mean (%.2f, %.2f) px, %d non-zero; field %d non-zero\n", frame,
				made ? "interpolated" : "reset (no interpolation)", std::chrono::duration<double, std::milli>(s1 - s0).count(), fw, fh,
				sx / (fw * fh), sy / (fw * fh), nz, fieldnz);
			// FG_DUMP_FLOW=1: the last frame's optical flow at level 0 (int16 x, y a block, row by row) in OUT.flow
			if (frame == frames - 1 && std::getenv("FG_DUMP_FLOW"))
				if (FILE* f = std::fopen((std::string(out) + ".flow").c_str(), "wb"))
				{
					std::fwrite(flow, 4, size_t(fw) * fh, f);
					std::fclose(f);
				}
		}
		if (made && frame == frames - 1)
		{
			picture o = a;
			std::memcpy(o.rgba.data(), map, bytes);
			for (size_t i = 3; i < o.rgba.size(); i += 4) o.rgba[i] = 255;
			save(out, o);
			std::printf("wrote %s\n", out);
			if (have_truth)
			{
				const picture plain = blend(a, b);
				std::printf("mean abs error against the true middle frame: interpolated %.2f, plain blend of A and B %.2f, A %.2f, B %.2f\n",
					diff(o, truth, 24), diff(plain, truth, 24), diff(a, truth, 24), diff(b, truth, 24));
				save(std::string(out) + ".truth.png", truth);
				save(std::string(out) + ".blend.png", plain);
				if (!reg.empty())
				{
					static const char* names[4] = {"rest", "head edge", "HUD", "square edge"};
					double eo[4], eb[4], ea[4], ebb[4];
					int n[4];
					region_errors(o, truth, reg, eo, n);
					region_errors(plain, truth, reg, eb, n);
					region_errors(a, truth, reg, ea, n);
					region_errors(b, truth, reg, ebb, n);
					std::printf("by region (mean abs error, 0-255):   result   blend       A       B   pixels\n");
					for (int i = 0; i < 4; i++)
						if (n[i]) std::printf("  %-12s                    %7.2f %7.2f %7.2f %7.2f %8d\n", names[i], eo[i], eb[i], ea[i], ebb[i], n[i]);
					save(std::string(out) + ".cmp.png", comparison({&a, &b, &truth, &plain, &o}, boxes));
				}
			}

			// m74: the other frames generated between the same two (--gen), each against its own true frame for a scene
			for (int i = 1; i < gen; i++)
			{
				VkCommandBuffer cmd2 = g.begin();
				if (!fg.interpolate_next(cmd2, static_cast<uint32_t>(i))) die("interpolate_next");
				download(cmd2, fg.output_image(), staging, a.w, a.h);
				g.submit(cmd2);
				picture oi = a;
				std::memcpy(oi.rgba.data(), map, bytes);
				for (size_t k = 3; k < oi.rgba.size(); k += 4) oi.rgba[k] = 255;
				const std::string name = std::string(out) + "." + std::to_string(i) + ".png";
				save(name, oi);
				std::printf("wrote %s (at %.3f of the way)\n", name.c_str(), (i + 1.0) / (gen + 1));
				if (scene)
				{
					const picture ti = render(*scene, frames - 2 + (i + 1.0) / (gen + 1), nullptr);
					std::printf("  mean abs error against its true frame: %.2f (plain blend %.2f)\n", diff(oi, ti, 24), diff(blend(a, b), ti, 24));
					if (!reg.empty())
					{
						static const char* names[4] = {"rest", "head edge", "HUD", "square edge"};
						double eo[4];
						int n[4];
						std::vector<unsigned char> ids;
						render(*scene, frames - 2 + (i + 1.0) / (gen + 1), &ids);
						region_errors(oi, ti, regions(ids, a.w, a.h), eo, n);
						for (int r = 0; r < 4; r++)
							if (n[r]) std::printf("  %-12s %7.2f\n", names[r], eo[r]);
					}
				}
			}
		}
		fg.advance();
	}

	{
		double mean_ms = 0, max_ms = 0;
		uint32_t count = 0;
		if (fg.take_gpu_time(mean_ms, max_ms, count))
			std::printf("GPU time of the passes: %.2f ms a frame (most %.2f, %u frames)\n", mean_ms, max_ms, count);
		else
			std::printf("GPU time of the passes: not measured\n");
	}

	fg.destroy();
	vkDestroyBuffer(g.device, staging, nullptr);
	vkFreeMemory(g.device, staging_mem, nullptr);
	vkDestroyCommandPool(g.device, g.pool, nullptr);
	vkDestroyDevice(g.device, nullptr);
	vkDestroyInstance(g.instance, nullptr);
	return 0;
}
