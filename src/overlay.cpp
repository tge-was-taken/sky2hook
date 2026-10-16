#include "overlay.h"

#include "common.h" // guest, RwCamera, RpLight, RwFrame, glad
#include "sky2/addresses.h" // sky2::addresses(): main camera/light globals + debug-draw flags
#include "device.h" // the Device whose queue, renderer and shaders this overlay inspects
#include "render_queue.h"
#include "texture_manager.h" // active camera + light addresses
#include "renderer.h" // frame stats, GPU memory, the frame limiter
#include "enhancements/msaa.h" // multisampling for the display and off-screen passes
#include "enhancements/shadow_detail.h" // the shadow render-target scale

#include <imgui.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

// The thread-local implicit ImGui context (see the GImGui override in imgui/imconfig.h). One
// definition for the whole program; each thread that uses ImGui gets its own current context.
thread_local ImGuiContext* GImGuiTLS = nullptr;

namespace sky2
{
	namespace
	{

		const char* kVS = "#version 450 core\n"
						  "layout(location=0) in vec2 aPos;\n"
						  "layout(location=1) in vec2 aUV;\n"
						  "layout(location=2) in vec4 aCol;\n"
						  "uniform mat4 uProj; out vec2 vUV; out vec4 vCol;\n"
						  "void main(){ vUV=aUV; vCol=aCol; gl_Position=uProj*vec4(aPos,0,1); }\n";
		const char* kFS = "#version 450 core\n"
						  "in vec2 vUV; in vec4 vCol; uniform sampler2D uTex; out vec4 o;\n"
						  "void main(){ o = vCol * texture(uTex, vUV); }\n";

		GLuint compile(GLenum type, const char* src)
		{
			GLuint s = glCreateShader(type);
			glShaderSource(s, 1, &src, nullptr);
			glCompileShader(s);
			return s;
		}

		bool initGL(ImguiOverlay& ui)
		{
			ui.prog = glCreateProgram();
			glAttachShader(ui.prog, compile(GL_VERTEX_SHADER, kVS));
			glAttachShader(ui.prog, compile(GL_FRAGMENT_SHADER, kFS));
			glLinkProgram(ui.prog);
			GLint linked = GL_FALSE;
			glGetProgramiv(ui.prog, GL_LINK_STATUS, &linked);
			if (!linked)
			{
				std::fprintf(stderr, "[sky2] imgui shader failed to link; overlay disabled\n");
				return false;
			}
			ui.uProj = glGetUniformLocation(ui.prog, "uProj");
			ui.uTex = glGetUniformLocation(ui.prog, "uTex");

			glGenVertexArrays(1, &ui.vao);
			glGenBuffers(1, &ui.vbo);
			glGenBuffers(1, &ui.ebo);
			glBindVertexArray(ui.vao);
			glBindBuffer(GL_ARRAY_BUFFER, ui.vbo);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui.ebo);
			const GLsizei st = sizeof(ImDrawVert);
			glEnableVertexAttribArray(0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, st, (void*)offsetof(ImDrawVert, pos));
			glEnableVertexAttribArray(1);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, st, (void*)offsetof(ImDrawVert, uv));
			glEnableVertexAttribArray(2);
			glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)offsetof(ImDrawVert, col));
			glBindVertexArray(0);
			return true;
		}

		// 1.92 texture requests: the atlas font (and any user texture) is created/updated/destroyed here.
		void updateTextures(ImDrawData* dd)
		{
			if (!dd->Textures)
				return;
			for (ImTextureData* tex : *dd->Textures)
			{
				// Match the GL format to the atlas format: uploading an Alpha8 atlas as GL_RGBA would
				// read 4x its buffer (out of bounds). Swizzle Alpha8 so texture() yields (1,1,1,a).
				const bool alpha8 = tex->Format == ImTextureFormat_Alpha8;
				const GLint internal = alpha8 ? GL_R8 : GL_RGBA8;
				const GLenum fmt = alpha8 ? GL_RED : GL_RGBA;
				if (tex->Status == ImTextureStatus_WantCreate)
				{
					GLuint id = 0;
					glGenTextures(1, &id);
					glBindTexture(GL_TEXTURE_2D, id);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					if (alpha8)
					{
						const GLint sw[4] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
						glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
					}
					glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
					glTexImage2D(GL_TEXTURE_2D, 0, internal, tex->Width, tex->Height, 0, fmt,
						GL_UNSIGNED_BYTE, tex->GetPixels());
					tex->SetTexID((ImTextureID)(intptr_t)id);
					tex->SetStatus(ImTextureStatus_OK);
				}
				else if (tex->Status == ImTextureStatus_WantUpdates)
				{
					glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)tex->TexID);
					glPixelStorei(GL_UNPACK_ROW_LENGTH, tex->Width);
					for (const ImTextureRect& r : tex->Updates)
						glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, fmt, GL_UNSIGNED_BYTE,
							tex->GetPixelsAt(r.x, r.y));
					glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
					tex->SetStatus(ImTextureStatus_OK);
				}
				else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
				{
					GLuint id = (GLuint)(intptr_t)tex->TexID;
					glDeleteTextures(1, &id);
					tex->SetTexID(ImTextureID_Invalid);
					tex->SetStatus(ImTextureStatus_Destroyed);
				}
			}
		}

		void renderDrawData(ImguiOverlay& ui, ImDrawData* dd, int fbW, int fbH)
		{
			updateTextures(dd);
			if (dd->CmdListsCount == 0 || fbW <= 0 || fbH <= 0)
				return;

			glBindFramebuffer(GL_FRAMEBUFFER, 0); // draw over the just-presented frame in the default FBO
			glEnable(GL_BLEND);
			glBlendEquation(GL_FUNC_ADD);
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			glDisable(GL_CULL_FACE);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			glEnable(GL_SCISSOR_TEST);
			glViewport(0, 0, fbW, fbH);

			const float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
			const float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
			const float proj[16] = {2.0f / (R - L), 0, 0, 0, 0, 2.0f / (T - B), 0, 0, 0, 0, -1, 0,
				(R + L) / (L - R), (T + B) / (B - T), 0, 1};
			glUseProgram(ui.prog);
			glUniformMatrix4fv(ui.uProj, 1, GL_FALSE, proj);
			glUniform1i(ui.uTex, 0);
			glActiveTexture(GL_TEXTURE0);
			glBindVertexArray(ui.vao);

			const ImVec2 pos = dd->DisplayPos, scale = dd->FramebufferScale;
			for (int i = 0; i < dd->CmdListsCount; i++)
			{
				const ImDrawList* cl = dd->CmdLists[i];
				glBindBuffer(GL_ARRAY_BUFFER, ui.vbo);
				glBufferData(GL_ARRAY_BUFFER, cl->VtxBuffer.Size * (int)sizeof(ImDrawVert),
					cl->VtxBuffer.Data, GL_STREAM_DRAW);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ui.ebo);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, cl->IdxBuffer.Size * (int)sizeof(ImDrawIdx),
					cl->IdxBuffer.Data, GL_STREAM_DRAW);

				for (int c = 0; c < cl->CmdBuffer.Size; c++)
				{
					const ImDrawCmd& cmd = cl->CmdBuffer[c];
					const float x0 = (cmd.ClipRect.x - pos.x) * scale.x;
					const float y0 = (cmd.ClipRect.y - pos.y) * scale.y;
					const float x1 = (cmd.ClipRect.z - pos.x) * scale.x;
					const float y1 = (cmd.ClipRect.w - pos.y) * scale.y;
					if (x1 <= x0 || y1 <= y0)
						continue;
					glScissor((int)x0, (int)(fbH - y1), (int)(x1 - x0), (int)(y1 - y0));
					glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd.GetTexID());
					glDrawElementsBaseVertex(GL_TRIANGLES, cmd.ElemCount,
						sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
						(void*)(intptr_t)(cmd.IdxOffset * sizeof(ImDrawIdx)), cmd.VtxOffset);
				}
			}
			glDisable(GL_SCISSOR_TEST);
			glBindVertexArray(0);
		}

		ImGuiKey sdlKey(SDL_Keycode k)
		{
			switch (k)
			{
				case SDLK_TAB: return ImGuiKey_Tab;
				case SDLK_LEFT: return ImGuiKey_LeftArrow;
				case SDLK_RIGHT: return ImGuiKey_RightArrow;
				case SDLK_UP: return ImGuiKey_UpArrow;
				case SDLK_DOWN: return ImGuiKey_DownArrow;
				case SDLK_BACKSPACE: return ImGuiKey_Backspace;
				case SDLK_DELETE: return ImGuiKey_Delete;
				case SDLK_RETURN: return ImGuiKey_Enter;
				case SDLK_ESCAPE: return ImGuiKey_Escape;
				case SDLK_HOME: return ImGuiKey_Home;
				case SDLK_END: return ImGuiKey_End;
				default: return ImGuiKey_None;
			}
		}

		void feedInput(ImguiOverlay& ui, int fbW, int fbH)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2((float)fbW, (float)fbH);
			io.DeltaTime = 1.0f / 60.0f;

			std::vector<QueuedEvent> evs;
			{
				std::lock_guard<std::mutex> lk(ui.evMutex);
				evs.swap(ui.events);
			}
			for (const QueuedEvent& qe : evs)
			{
				const SDL_Event& e = qe.ev;
				switch (e.type)
				{
					case SDL_EVENT_MOUSE_MOTION:
						io.AddMousePosEvent(e.motion.x, e.motion.y);
						break;
					case SDL_EVENT_MOUSE_BUTTON_DOWN:
					case SDL_EVENT_MOUSE_BUTTON_UP:
					{
						const int b = e.button.button == SDL_BUTTON_LEFT ? 0
							: e.button.button == SDL_BUTTON_RIGHT		 ? 1
							: e.button.button == SDL_BUTTON_MIDDLE		 ? 2
																		 : -1;
						if (b >= 0)
							io.AddMouseButtonEvent(b, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
						break;
					}
					case SDL_EVENT_MOUSE_WHEEL:
						io.AddMouseWheelEvent(e.wheel.x, e.wheel.y);
						break;
					case SDL_EVENT_TEXT_INPUT:
						io.AddInputCharactersUTF8(qe.text.c_str()); // copied at push time (see QueuedEvent)
						break;
					case SDL_EVENT_KEY_DOWN:
					case SDL_EVENT_KEY_UP:
					{
						const bool down = e.type == SDL_EVENT_KEY_DOWN;
						io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
						io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
						io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
						const ImGuiKey ik = sdlKey(e.key.key);
						if (ik != ImGuiKey_None)
							io.AddKeyEvent(ik, down);
						break;
					}
					default:
						break;
				}
			}
		}

		// ---- the debug window: live RwCamera + RpLights (read from guest RAM, tweaks written back) ----

		uint32_t guestU32(uint32_t addr)
		{
			return (addr && guest::ready()) ? *guest::ptr<uint32_t>(addr) : 0;
		}

		uint32_t mainCameraAddr(ImguiOverlay& ui)
		{
			uint32_t a = guestU32(sky2::addresses().sMainCamera);
			return a ? a : ui.device->queue.activeCamera; // fall back before the global is set
		}

		// The camera the sections act on: the selected pass's, else the main camera.
		uint32_t inspectedCameraAddr(ImguiOverlay& ui)
		{
			const std::vector<Renderer::PassInfo>& passes = ui.device->renderer.passes;
			if (ui.selectedPass >= 0 && ui.selectedPass < static_cast<int>(passes.size()))
				return passes[static_cast<size_t>(ui.selectedPass)].camera;
			return mainCameraAddr(ui);
		}

		const char* passKind(const Renderer::PassInfo& p)
		{
			return p.isDisplay ? "display" : "offscreen";
		}

		// One row per pass of the last frame: what it targets, what it drew, and what it cleared.
		// Selecting a row points the Camera and Lights sections at that pass; the checkbox switches the
		// pass off entirely, which is the quickest way to see what it contributes.
		void passesSection(ImguiOverlay& ui)
		{
			Renderer& r = ui.device->renderer;
			std::vector<Renderer::PassInfo>& passes = r.passes;
			if (passes.empty())
			{
				ImGui::TextDisabled("no passes yet");
				return;
			}
			r.passDisabled.resize(passes.size(), 0);
			if (ImGui::SmallButton("all on"))
				std::fill(r.passDisabled.begin(), r.passDisabled.end(), uint8_t(0));
			ImGui::SameLine();
			if (ImGui::SmallButton("solo selected") && ui.selectedPass >= 0)
				for (size_t i = 0; i < r.passDisabled.size(); i++)
					r.passDisabled[i] = (static_cast<int>(i) == ui.selectedPass) ? 0 : 1;
			ImGui::SameLine();
			ImGui::TextDisabled("(%zu passes)", passes.size());
			if (ImGui::Selectable("main camera (no pass)", ui.selectedPass < 0))
				ui.selectedPass = -1;
			for (size_t i = 0; i < passes.size(); i++)
			{
				const Renderer::PassInfo& p = passes[i];
				ImGui::PushID(static_cast<int>(i));
				bool on = !r.passDisabled[i];
				if (ImGui::Checkbox("##on", &on))
					r.passDisabled[i] = on ? 0 : 1;
				ImGui::SameLine();
				char label[176];
				std::snprintf(label, sizeof(label),
					"%2zu %-9s %dx%d  target 0x%08X  cam 0x%08X  %3d mesh  %3d 2D%s%s", i, passKind(p),
					p.w, p.h, p.target, p.camera, p.meshes, p.im2dBatches, p.clearColor ? "  clr:C" : "",
					p.clearDepth ? "  clr:Z" : "");
				if (ImGui::Selectable(label, ui.selectedPass == static_cast<int>(i)))
					ui.selectedPass = static_cast<int>(i);
				ImGui::PopID();
			}
			// What the selected pass rendered. For an off-screen target this is the texture the game
			// samples later, so a black or stale preview here explains a wrong-looking effect downstream.
			if (ui.selectedPass < 0 || ui.selectedPass >= static_cast<int>(passes.size()))
				return;
			const Renderer::PassInfo& sel = passes[static_cast<size_t>(ui.selectedPass)];
			const Renderer::PassPreview pv = r.passPreview(sel);
			if (!pv.tex || pv.w <= 0 || pv.h <= 0)
			{
				ImGui::TextDisabled(sel.isDisplay ? "display pass: it is the window"
												  : "no rendered texture for this pass");
				return;
			}
			const float width = std::min(320.0f, static_cast<float>(pv.w));
			const ImVec2 size(width, width * static_cast<float>(pv.h) / static_cast<float>(pv.w));
			const ImVec2 uv0(0.0f, pv.flipV ? 1.0f : 0.0f), uv1(1.0f, pv.flipV ? 0.0f : 1.0f);
			ImGui::Text("target %dx%d", pv.w, pv.h);
			// An effect target is mostly black and transparent, which is invisible against the
			// panel. Checkerboard behind it shows through wherever alpha is low, and the frame marks the
			// extent, so "all black" and "nothing rendered" stay distinguishable.
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			constexpr float CHECKER = 8.0f;
			for (float y = 0.0f; y < size.y; y += CHECKER)
			{
				for (float x = 0.0f; x < size.x; x += CHECKER)
				{
					const bool odd = (static_cast<int>(x / CHECKER) + static_cast<int>(y / CHECKER)) & 1;
					dl->AddRectFilled(ImVec2(origin.x + x, origin.y + y),
						ImVec2(origin.x + std::min(x + CHECKER, size.x), origin.y + std::min(y + CHECKER, size.y)),
						odd ? IM_COL32(70, 70, 76, 255) : IM_COL32(48, 48, 54, 255));
				}
			}
			ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(pv.tex)), size, uv0, uv1);
			dl->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(150, 150, 160, 255));
		}

		void cameraSection(ImguiOverlay& ui)
		{
			const uint32_t camAddr = inspectedCameraAddr(ui);
			const std::vector<Renderer::PassInfo>& passes = ui.device->renderer.passes;
			if (ui.selectedPass >= 0 && ui.selectedPass < static_cast<int>(passes.size()))
				ImGui::Text("RwCamera 0x%08X (pass %d, %s)", camAddr, ui.selectedPass,
					passKind(passes[static_cast<size_t>(ui.selectedPass)]));
			else
				ImGui::Text("RwCamera 0x%08X (main camera)", camAddr);
			if (!camAddr || !guest::ready())
				return;
			const auto cam = guest::ptr<RwCamera>(camAddr);
			ImGui::Text("projection: %s", cam->projectionType == 1 ? "perspective" : cam->projectionType == 2 ? "parallel" : "?");
			float vw[2] = {cam->viewWindow.x, cam->viewWindow.y};
			float nearP = cam->nearPlane, farP = cam->farPlane;
			bool ch = false;
			ch |= ImGui::DragFloat2("viewWindow", vw, 0.001f);
			ch |= ImGui::DragFloat("near", &nearP, 0.5f);
			ch |= ImGui::DragFloat("far", &farP, 5.0f);
			if (ch)
			{
				cam->viewWindow.x = vw[0];
				cam->viewWindow.y = vw[1];
				cam->nearPlane = nearP;
				cam->farPlane = farP;
				if (ui.camLock.active)
				{
					ui.camLock.vw[0] = vw[0];
					ui.camLock.vw[1] = vw[1];
					ui.camLock.nearP = nearP;
					ui.camLock.farP = farP;
				}
			}
			if (ImGui::Checkbox("lock##cam", &ui.camLock.active) && ui.camLock.active)
			{
				ui.camLock.vw[0] = vw[0];
				ui.camLock.vw[1] = vw[1];
				ui.camLock.nearP = nearP;
				ui.camLock.farP = farP;
			}
			ImGui::Text("fogPlane %.1f", cam->fogPlane);
			const uint32_t frame = cam->object.object.parent.addr;
			if (frame)
			{
				const RwV3d& p = guest::ptr<RwFrame>(frame)->ltm.pos;
				ImGui::Text("pos  %.1f, %.1f, %.1f", p.x, p.y, p.z);
			}
		}

		// One light, read from its global pointer (sky2::addresses). `slot` selects the gatherLights slot; the
		// hold is scoped to `pass` (the selected pass, or -1 for every pass), so the same light can be
		// held at one colour in one pass and left alone elsewhere. Unheld edits write the guest object,
		// which the game overwrites on its next light update: hold it to make an edit stick.
		void lightRow(ImguiOverlay& ui, int slot, const char* label, uint32_t ptrGlobalAddr, int pass)
		{
			const uint32_t objAddr = guestU32(ptrGlobalAddr);
			RenderQueue& queue = ui.device->queue;
			ImGui::PushID(slot);
			if (!objAddr)
			{
				ImGui::TextDisabled("%s: null", label);
				ImGui::PopID();
				return;
			}
			const auto light = guest::ptr<RpLight>(objAddr);
			const std::optional<glm::vec3> held = queue.lightLock(pass, slot);
			float col[3] = {light->color.red, light->color.green, light->color.blue};
			if (held)
			{
				col[0] = held->x;
				col[1] = held->y;
				col[2] = held->z;
			}
			if (ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
			{
				if (held)
				{
					queue.setLightLock(pass, slot, glm::vec3(col[0], col[1], col[2]));
				}
				else
				{
					light->color.red = col[0];
					light->color.green = col[1];
					light->color.blue = col[2];
				}
			}
			ImGui::SameLine();
			bool active = held.has_value();
			if (ImGui::Checkbox("hold", &active))
			{
				if (active)
					queue.setLightLock(pass, slot, glm::vec3(col[0], col[1], col[2]));
				else
					queue.clearLightLock(pass, slot);
			}
			ImGui::PopID();
		}

		void lightsSection(ImguiOverlay& ui)
		{
			RenderQueue& q = ui.device->queue;
			if (!guest::ready())
				return;
			const auto& g = sky2::addresses();
			const std::vector<Renderer::PassInfo>& passes = ui.device->renderer.passes;
			const bool perPass = ui.selectedPass >= 0 && ui.selectedPass < static_cast<int>(passes.size());
			// Always a render-thread copy. RenderQueue::currentLights is 552 bytes the EE thread
			// rewrites per object, so reading it from here returned a torn mix of two objects' lighting.
			static const FrameLights NO_LIGHTS;
			const size_t which = perPass ? static_cast<size_t>(ui.selectedPass) : passes.size() - 1;
			const FrameLights& fl = passes.empty() ? NO_LIGHTS : passes[which].lights;
			ImGui::TextDisabled("%s: amb %.2f,%.2f,%.2f  dirCount=%d  localCount=%d",
				perPass ? "pass lights (first mesh)" : "last pass (first mesh)", fl.ambient.r,
				fl.ambient.g, fl.ambient.b, fl.dirCount, fl.localCount);
			// A hold applies to the pass selected above, or to every pass when none is.
			const int scope = perPass ? ui.selectedPass : -1;
			if (perPass)
				ImGui::TextDisabled("holds apply to pass %d only", scope);
			else
				ImGui::TextDisabled("holds apply to every pass (select one above to scope them)");
			lightRow(ui, 0, "ambient", g.sAmbientLight, scope);
			lightRow(ui, 1, "directional", g.sDirectionalLight, scope);
			lightRow(ui, 2, "directional 2", g.sDirectionalLight2, scope);
			if (q.anyLightLockHeld())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("release all"))
					q.clearLightLocks();
			}
		}

		void toggleRow(ImguiOverlay& ui, int i, const char* label, uint32_t addr)
		{
			if (!addr || !guest::ready())
				return;
			ImGui::PushID(i);
			const auto p = guest::ptr<int32_t>(addr);
			int32_t v = ui.togLock[i].active ? ui.togLock[i].value : *p;
			bool on = v != 0;
			if (ImGui::Checkbox(label, &on))
			{
				v = on ? 1 : 0;
				if (ui.togLock[i].active)
					ui.togLock[i].value = v;
				else
					*p = v;
			}
			ImGui::SameLine();
			ImGui::Checkbox("lock", &ui.togLock[i].active);
			if (ui.togLock[i].active)
				ui.togLock[i].value = v;
			ImGui::SameLine();
			ImGui::TextDisabled("0x%08X=%d", addr, *p);
			ImGui::PopID();
		}

		void togglesSection(ImguiOverlay& ui)
		{
			const auto& g = sky2::addresses();
			toggleRow(ui, 0, "bounding spheres", g.debugDrawBoundingSpheres);
			toggleRow(ui, 1, "draw flag 7CE158", g.unk_7CE158);
		}

		// Re-assert the locked camera/toggle values into guest RAM every frame (runs whether or not the
		// window is shown). Light locks are applied at gather time in gatherLights, so only their
		// object address is refreshed here in case the global now points at a new light.
		void enforceLocks(ImguiOverlay& ui)
		{
			if (!guest::ready())
				return;
			const auto& g = sky2::addresses();
			if (ui.camLock.active)
			{
				const uint32_t camAddr = mainCameraAddr(ui);
				if (camAddr)
				{
					const auto cam = guest::ptr<RwCamera>(camAddr);
					cam->viewWindow.x = ui.camLock.vw[0];
					cam->viewWindow.y = ui.camLock.vw[1];
					cam->nearPlane = ui.camLock.nearP;
					cam->farPlane = ui.camLock.farP;
				}
			}
			// Light holds need no work here: gatherLights substitutes them per pass as it builds.
			const uint32_t togAddr[2] = {g.debugDrawBoundingSpheres, g.unk_7CE158};
			for (int i = 0; i < 2; i++)
				if (ui.togLock[i].active && togAddr[i])
					*guest::ptr<int32_t>(togAddr[i]) = ui.togLock[i].value;
		}

		// Changes to how the game feels rather than how faithfully it renders. Each is off unless its
		// config key enabled it at startup, because they hook guest code and a hook is not free.
		void enhancementsSection(ImguiOverlay& ui)
		{
			Renderer& r = ui.device->renderer;
			bool interpolating = r.interp.enabled();
			if (ImGui::Checkbox("interpolate motion (30 Hz -> present rate)", &interpolating))
			{
				r.interp.setEnabled(interpolating);
			}
			if (interpolating)
			{
				const Interpolator::Stats& st = r.interp.stats;
				ImGui::SameLine();
				ImGui::TextDisabled("a %.2f", r.interp.alpha());
				ImGui::Text("  mesh %d moved / %d snap    skin %d / %d snap", st.meshInterpolated,
					st.meshSnapped, st.bonesInterpolated, st.bonesSnapped);
				ImGui::Text("  2D %d blend / %d snap    tick %.1f ms", st.im2dBlended, st.im2dSnapped,
					st.tickMs);
			}
			ImGui::Separator();
			// Rebuilding the FBO is what acquireTarget does on a size change, so this can move live.
			int shadowScale = enhancements::shadowDetail.load(std::memory_order_relaxed);
			if (ImGui::SliderInt("shadow detail", &shadowScale, 1, 8, "%dx"))
			{
				enhancements::shadowDetail.store(shadowScale, std::memory_order_relaxed);
			}
			bool vsync = ui.device->renderer.vsync.load();
			if (ImGui::Checkbox("vsync (present on the display's cadence)", &vsync))
			{
				ui.device->renderer.vsync.store(vsync);
			}
			int msaa = enhancements::displaySamples.load(std::memory_order_relaxed);
			if (ImGui::SliderInt("MSAA", &msaa, 1, 8, "%dx"))
			{
				enhancements::displaySamples.store(msaa, std::memory_order_relaxed);
			}
			int msaaOff = enhancements::offscreenSamples.load(std::memory_order_relaxed);
			if (ImGui::SliderInt("MSAA (shadows)", &msaaOff, 1, 8, "%dx"))
			{
				enhancements::offscreenSamples.store(msaaOff, std::memory_order_relaxed);
			}
			for (const api::OverlaySlider& row : api::overlaySliders())
			{
				if (!row.armed)
				{
					if (row.note)
						ImGui::TextDisabled("%s", row.note);
					continue;
				}
				float v = row.value->load(std::memory_order_relaxed);
				if (ImGui::SliderFloat(row.label, &v, row.min, row.max, "%.2fx"))
					row.value->store(v, std::memory_order_relaxed);
			}
		}

		// Frame rate, where the time goes, and what the renderer is holding on the GPU.
		void statsSection(ImguiOverlay& ui)
		{
			Renderer& r = ui.device->renderer;
			const Renderer::Stats& st = r.stats;
			ImGui::TextUnformatted(r.glRenderer.empty() ? "GL renderer: ?" : r.glRenderer.c_str());
			ImGui::Text("%.1f fps  (%.2f ms/frame)", st.fps, st.frameMs);
			// With the limiter on, frameMs is the cap; renderMs is what the frame actually costs, so it
			// says what the rate would be if uncapped.
			if (st.renderMs > 0.0f)
			{
				ImGui::Text("render %.2f ms  ->  %.0f fps uncapped", st.renderMs, 1000.0f / st.renderMs);
			}
			bool limiter = r.frameLimiter.load();
			if (ImGui::Checkbox("frame limiter (Tab)", &limiter))
			{
				api::toggleFrameLimiter();
			}
			ImGui::SameLine();
			// The present rate is the render thread's own pacing and does not touch emulation speed,
			// so raising it past the guest's 30 Hz just re-presents the held frame more often.
			ImGui::SetNextItemWidth(110.0f);
			ImGui::DragInt("fps##cap", &r.frameLimitRate, 1.0f, 15, 480, "%d fps");
			ImGui::Text("passes %d   meshes %d   2D batches %d", st.passes, st.meshes, st.im2dBatches);
			ImGui::Separator();
			const Renderer::MemoryUse m = r.memoryUse();
			const auto mb = [](size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
			ImGui::Text("GPU memory %.1f MB", mb(m.total()));
			ImGui::Text("  textures   %6.1f MB  (%zu)", mb(m.textureBytes), m.textureCount);
			ImGui::Text("  geometry   %6.1f MB  (%zu)", mb(m.geometryBytes), m.geometryCount);
			ImGui::Text("  indices    %6.1f MB  (%zu)", mb(m.indexBytes), m.indexCount);
			ImGui::Text("  targets    %6.1f MB  (%zu)", mb(m.targetBytes), m.targetCount);
			ImGui::Text("  screen     %6.1f MB", mb(m.screenBytes));
		}

		// Advance every rolling window once per rendered frame. Deliberately not inside the panels: a
		// history that only fills while its section is expanded is a history with holes in it, and the
		// spikes worth catching happen while you are looking at something else.
		void sampleStats(ImguiOverlay& ui)
		{
			const auto get = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
			const auto mb = [](size_t bytes) { return float(double(bytes) / (1024.0 * 1024.0)); };

			const TextureManager::Stats& t = ui.device->textures.stats;
			ImguiOverlay::TexCost& tc = ui.texCost;
			tc.hash.sample(get(t.hashNs));
			tc.decode.sample(get(t.decodeNs));
			tc.upload.sample(get(t.uploadNs));
			tc.decodes.sample(get(t.decodes));
			tc.swept.sample(get(t.contentSwept));
			tc.resident.push(float(get(t.contentLive)));
			tc.cacheMb.push(mb(ui.device->textures.uploadedBytes()));

			const GeometryPool& pool = ui.device->geometry;
			const GeomStats& g = pool.stats;
			ImguiOverlay::GeomCost& gc = ui.geomCost;
			gc.parse.sample(get(g.atomics.ns) + get(g.sectors.ns));
			gc.upload.sample(get(g.uploadNs));
			gc.parses.sample(get(g.atomics.misses) + get(g.sectors.misses));
			gc.uploads.sample(get(g.vertUploads) + get(g.idxUploads));
			gc.creates.sample(get(g.bufferCreates));
			gc.evictions.sample(get(g.evictions));
			gc.im3dBatches.sample(get(g.im3dBatches));
			gc.poolMb.push(mb(pool.verts.bytes() + pool.indices.bytes()));
		}

		// A rolling window as a plot, with the current value written over it. Scaled to the window's own
		// peak so a quiet metric still shows its shape, with a floor so an all-zero window stays flat.
		void plotHistory(const char* label, const ImguiOverlay::History& h, const char* fmt)
		{
			char overlay[64];
			std::snprintf(overlay, sizeof(overlay), fmt, h.last);
			ImGui::PlotLines(label, h.v, ImguiOverlay::History::SAMPLES, h.head, overlay, 0.0f,
				std::max(h.windowPeak() * 1.1f, 1e-3f), ImVec2(0.0f, 34.0f));
		}

		// Where a texture bind goes: a hash of the guest pixels, then either a hit on content we already
		// hold or a decode plus an upload. Hits and memo hits are what keep the load spike down.
		void texturesSection(ImguiOverlay& ui)
		{
			const TextureManager::Stats& t = ui.device->textures.stats;
			const auto get = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
			ImGui::Text("decodes %llu", static_cast<unsigned long long>(get(t.decodes)));
			const TextureManager::CacheBreakdown b = ui.device->textures.breakdown();
			ImGui::Text("GL textures: %zu content (%.1f MB)  %zu streaming (%.1f MB)  %zu RT  %zu feedback",
				b.content, double(b.contentBytes) / (1024.0 * 1024.0), b.streaming,
				double(b.streamingBytes) / (1024.0 * 1024.0), b.renderTarget, b.feedback);
			ImGui::Text("streaming addresses %llu", static_cast<unsigned long long>(get(t.streamingAddrs)));
			ImGui::Text("content resident %llu   hits %llu   swept %llu   evictions %llu",
				static_cast<unsigned long long>(get(t.contentLive)),
				static_cast<unsigned long long>(get(t.contentHits)),
				static_cast<unsigned long long>(get(t.contentSwept)),
				static_cast<unsigned long long>(get(t.evictions)));

			// Where a loading spike actually goes. Hashing runs on every bind, decoding only on a
			// content miss, uploading only here on the render thread; each has a different fix.
			ImguiOverlay::TexCost& tc = ui.texCost;
			ImGui::Separator();
			ImGui::TextDisabled("cost per frame        now     peak      (last 5s)");
			ImGui::Text("  hash   (EE)   %7.2f  %7.2f ms", tc.hash.ms, tc.hash.peakMs);
			plotHistory("##texhash", tc.hash.hist, "%.2f ms hash");
			ImGui::Text("  decode (EE)   %7.2f  %7.2f ms", tc.decode.ms, tc.decode.peakMs);
			plotHistory("##texdecode", tc.decode.hist, "%.2f ms decode");
			ImGui::Text("  upload (GL)   %7.2f  %7.2f ms", tc.upload.ms, tc.upload.peakMs);
			plotHistory("##texupload", tc.upload.hist, "%.2f ms upload");
			ImGui::TextDisabled("over time");
			plotHistory("##texdecodes", tc.decodes.hist, "%.0f decodes/frame");
			plotHistory("##texswept", tc.swept.hist, "%.0f swept/frame");
			plotHistory("##texresident", tc.resident, "%.0f content resident");
			plotHistory("##texmb", tc.cacheMb, "%.1f MB uploaded textures");
			const uint64_t hashes = get(t.hashCalls), memo = get(t.hashMemoHits);
			ImGui::Text("hashed %.1f MB over %llu hashes", double(get(t.hashBytes)) / (1024.0 * 1024.0),
				static_cast<unsigned long long>(hashes));
			ImGui::Text("  memo hits %llu  (%.1f%% of binds skipped)", static_cast<unsigned long long>(memo),
				(hashes + memo) ? 100.0 * double(memo) / double(hashes + memo) : 0.0);
			if (ImGui::SmallButton("reset peaks"))
			{
				tc.hash.peakMs = tc.decode.peakMs = tc.upload.peakMs = 0.0f;
			}
		}

		// The same accounting for geometry: a parse turns guest vertex and index arrays into our vertex
		// layout on the EE thread, an upload hands the result to GL. Both are skipped by a cache hit, so
		// the hit rates are what say whether a spike is new geometry or the caches failing to hold.
		void geometrySection(ImguiOverlay& ui)
		{
			GeometryPool& pool = ui.device->geometry;
			const GeomStats& g = pool.stats;
			const auto get = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
			const auto parseLine = [&](const char* name, const GeomStats::Parse& p) {
				const uint64_t miss = get(p.misses), hit = get(p.hits);
				ImGui::Text("  %-8s %6llu parsed  %8llu cached  (%.1f%% hit)", name,
					static_cast<unsigned long long>(miss), static_cast<unsigned long long>(hit),
					(miss + hit) ? 100.0 * double(hit) / double(miss + hit) : 0.0);
				ImGui::Text("           %6llu verts   %8llu indices   %llu re-parsed unchanged",
					static_cast<unsigned long long>(get(p.verts)),
					static_cast<unsigned long long>(get(p.indices)),
					static_cast<unsigned long long>(get(p.unchanged)));
			};
			const auto mb = [](size_t bytes) { return double(bytes) / (1024.0 * 1024.0); };
			ImGui::TextDisabled("parses");
			parseLine("atomics", g.atomics);
			parseLine("sectors", g.sectors);
			ImGui::Text("  im3d     %6llu batches  %8llu with an identity transform  %.1f MB",
				static_cast<unsigned long long>(get(g.im3dBatches)),
				static_cast<unsigned long long>(get(g.im3dIdentityModel)),
				mb(get(g.im3dBytes)));
			ImGui::Text("pool  verts %zu live %.1f MB (%zu reused / %zu new)", pool.verts.liveCount(),
				mb(pool.verts.bytes()), pool.verts.reused(), pool.verts.allocated());
			ImGui::Text("      idx   %zu live %.1f MB (%zu reused / %zu new)", pool.indices.liveCount(),
				mb(pool.indices.bytes()), pool.indices.reused(), pool.indices.allocated());
			ImGui::Text("uploads %llu vertex + %llu index buffers, %.1f MB",
				static_cast<unsigned long long>(get(g.vertUploads)),
				static_cast<unsigned long long>(get(g.idxUploads)), mb(get(g.uploadBytes)));
			ImGui::Text("  into %llu created buffers   evictions %llu",
				static_cast<unsigned long long>(get(g.bufferCreates)),
				static_cast<unsigned long long>(get(g.evictions)));

			ImguiOverlay::GeomCost& gc = ui.geomCost;
			ImGui::Separator();
			ImGui::TextDisabled("cost per frame        now     peak      (last 5s)");
			ImGui::Text("  parse  (EE)   %7.2f  %7.2f ms", gc.parse.ms, gc.parse.peakMs);
			plotHistory("##geomparse", gc.parse.hist, "%.2f ms parse");
			ImGui::Text("  upload (GL)   %7.2f  %7.2f ms", gc.upload.ms, gc.upload.peakMs);
			plotHistory("##geomupload", gc.upload.hist, "%.2f ms upload");
			ImGui::TextDisabled("over time");
			plotHistory("##geomparses", gc.parses.hist, "%.0f parses/frame");
			plotHistory("##geomuploads", gc.uploads.hist, "%.0f uploads/frame");
			plotHistory("##geomcreates", gc.creates.hist, "%.0f buffers created/frame");
			plotHistory("##geomevict", gc.evictions.hist, "%.0f evictions/frame");
			plotHistory("##geomim3d", gc.im3dBatches.hist, "%.0f im3d batches/frame");
			plotHistory("##geompool", gc.poolMb, "%.1f MB geometry pool");
			if (ImGui::SmallButton("reset geometry peaks"))
			{
				gc.parse.peakMs = gc.upload.peakMs = 0.0f;
			}
		}

		void buildUI(ImguiOverlay& ui)
		{
			if (!ui.show)
				return;
			ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
			if (ImGui::Begin("sky2 debug", &ui.show))
			{
				int ch = ui.device->shaders.debugChannel();
				const char* kChannels[] = {"0 normal", "1 texture", "2 material", "3 ambient",
					"4 diffuse", "5 prelit", "6 light", "7 base", "8 texcoords", "9 normal"};
				if (ImGui::Combo("channel", &ch, kChannels, IM_ARRAYSIZE(kChannels)))
					ui.device->shaders.setDebugChannel(ch);
				ImGui::Separator();
				if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
					statsSection(ui);
				if (ImGui::CollapsingHeader("Passes", ImGuiTreeNodeFlags_DefaultOpen))
					passesSection(ui);
				if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
					cameraSection(ui);
				if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
					lightsSection(ui);
				if (ui.openAll)
					ImGui::SetNextItemOpen(true, ImGuiCond_Once);
				if (ImGui::CollapsingHeader("Textures"))
					texturesSection(ui);
				if (ui.openAll)
					ImGui::SetNextItemOpen(true, ImGuiCond_Once);
				if (ImGui::CollapsingHeader("Geometry"))
					geometrySection(ui);
				if (ImGui::CollapsingHeader("Enhancements", ImGuiTreeNodeFlags_DefaultOpen))
					enhancementsSection(ui);
				if (ImGui::CollapsingHeader("Debug draw", ImGuiTreeNodeFlags_DefaultOpen))
					togglesSection(ui);
			}
			ImGui::End();
		}
	} // namespace

	void ImguiOverlay::init()
	{
		if (sky2::env("NO_IMGUI"))
			return; // safety valve: run without the overlay
		if (ready)
			return;
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr; // no imgui.ini on disk
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // we service ImTextureData requests
		ImGui::StyleColorsDark();
		if (!initGL(*this))
		{
			ImGui::DestroyContext();
			return; // leave ready false: the overlay is disabled but the game keeps running
		}
		ready = true;
		show = openAll = sky2::env("IMGUI_OPEN") != nullptr; // open from the start, for a scripted run
		std::fprintf(stderr, "[sky2] imgui overlay ready; press ` in the \"P3FES\" window to toggle it\n");
	}

	void ImguiOverlay::shutdown()
	{
		if (!ready)
			return;
		if (prog) glDeleteProgram(prog);
		if (vao) glDeleteVertexArrays(1, &vao);
		if (vbo) glDeleteBuffers(1, &vbo);
		if (ebo) glDeleteBuffers(1, &ebo);
		ImGui::DestroyContext();
		ready = false;
	}

	void ImguiOverlay::frame(int fbWidth, int fbHeight)
	{
		if (!ready)
			return;
		enforceLocks(*this); // hold any locked values against the game's per-frame writes, shown or not
		sampleStats(*this); // likewise: the histories advance whether or not anyone is watching
		feedInput(*this, fbWidth, fbHeight);
		ImGui::NewFrame();
		buildUI(*this);
		ImGui::Render();
		const ImGuiIO& io = ImGui::GetIO();
		wantInput.store(io.WantCaptureMouse || io.WantCaptureKeyboard);
		renderDrawData(*this, ImGui::GetDrawData(), fbWidth, fbHeight);
		// Don't let any GL error from the overlay leak into the next sky2 frame's gl::checkGlErrors.
		while (glGetError() != GL_NO_ERROR)
			;
	}

	void ImguiOverlay::pushEvent(const void* sdlEvent)
	{
		const SDL_Event& e = *static_cast<const SDL_Event*>(sdlEvent);
		QueuedEvent qe;
		qe.ev = e;
		if (e.type == SDL_EVENT_TEXT_INPUT && e.text.text)
			qe.text = e.text.text; // copy now; the SDL-owned pointer is freed after event processing
		std::lock_guard<std::mutex> lk(evMutex);
		events.push_back(std::move(qe));
	}

	void ImguiOverlay::toggle()
	{
		show = !show;
		std::fprintf(stderr, "[sky2] imgui overlay %s\n", show ? "shown" : "hidden");
	}

	bool ImguiOverlay::wantsInput() const { return wantInput.load(); }


} // namespace sky2
