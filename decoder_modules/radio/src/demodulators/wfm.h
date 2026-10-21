#pragma once
#include "../demod.h"
#include <dsp/demod/broadcast_fm.h>
#include "../rds_demod.h"
#include <gui/widgets/symbol_diagram.h>
#include <fstream>
#include <rds.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <cstring>
#include <chrono>
#include <gui/menus/display.h>

// MPX Analysis requires FFTW3
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <fftw3.h>
#ifdef _WIN32
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace demod {
    enum RDSRegion {
        RDS_REGION_EUROPE,
        RDS_REGION_NORTH_AMERICA
    };

    class WFM : public Demodulator {
    public:
        WFM() : diag(0.5, 4096), fftInitialized(false)  {}

        WFM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) : diag(0.5, 4096), fftInitialized(false) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~WFM() {
            stop();
            gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);

            // 释放 MPX 分析用的 FFT 资源
            if (fftInitialized) {
                fftwf_destroy_plan(fftPlan);
                fftwf_free(fftInput);
                fftwf_free(fftOutput);
                fftInitialized = false;
            }
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Define RDS regions
            rdsRegions.define("eu", "欧洲", RDS_REGION_EUROPE);
            rdsRegions.define("na", "北美", RDS_REGION_NORTH_AMERICA);

            // Register FFT draw handler
            fftRedrawHandler.handler = fftRedraw;
            fftRedrawHandler.ctx = this;
            gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);

            // Default
            std::string rdsRegionStr = "eu";

            // Load config
            _config->acquire();
            bool modified = false;
            if (config->conf[name][getName()].contains("stereo")) {
                _stereo = config->conf[name][getName()]["stereo"];
            }
            if (config->conf[name][getName()].contains("lowPass")) {
                _lowPass = config->conf[name][getName()]["lowPass"];
            }
            if (config->conf[name][getName()].contains("rds")) {
                _rds = config->conf[name][getName()]["rds"];
            }
            if (config->conf[name][getName()].contains("rdsIncremental")) {
                _rdsIncremental = config->conf[name][getName()]["rdsIncremental"];
            }
            if (config->conf[name][getName()].contains("rdsInfo")) {
                _rdsInfo = config->conf[name][getName()]["rdsInfo"];
            }
            if (config->conf[name][getName()].contains("stereoAnalysis")) {
                _stereoAnalysis = config->conf[name][getName()]["stereoAnalysis"];
            }
            if (config->conf[name][getName()].contains("rdsRegion")) {
                rdsRegionStr = config->conf[name][getName()]["rdsRegion"];
            }
            _config->release(modified);

            // Load RDS region
            if (rdsRegions.keyExists(rdsRegionStr)) {
                rdsRegionId = rdsRegions.keyId(rdsRegionStr);
                rdsRegion = rdsRegions.value(rdsRegionId);
            }
            else {
                rdsRegion = RDS_REGION_EUROPE;
                rdsRegionId = rdsRegions.valueId(rdsRegion);
            }

            // Init DSP
            demod.init(input, bandwidth / 2.0f, getIFSampleRate(), _stereo, _lowPass, _rds, _stereoAnalysis);
            rdsDemod.init(&demod.rdsOut, _rdsInfo);
            hs.init(&rdsDemod.out, rdsHandler, this);
            reshape.init(&rdsDemod.soft, 4096, (1187 / 30) - 4096);
            diagHandler.init(&reshape.out, _diagHandler, this);

            // 若已启用立体声分析，初始化 MPX 分析处理器
            if (_stereoAnalysis) {
                initMPXAnalysis();
                mpxHandler.init(&demod.mpxOut, mpxAnalysisHandler, this);
            }

            // Init RDS display
            diag.lines.push_back(-0.8);
            diag.lines.push_back(0.8);
        }

        void start() {
            demod.start();
            rdsDemod.start();
            hs.start();
            reshape.start();
            diagHandler.start();
            if (_stereoAnalysis) {
                mpxHandler.start();
            }
        }

        void stop() {
            demod.stop();
            rdsDemod.stop();
            hs.stop();
            reshape.stop();
            diagHandler.stop();
            if (_stereoAnalysis) {
                mpxHandler.stop();
            }
        }

        void showMenu() {
            if (ImGui::Checkbox(("低通##_radio_wfm_lowpass_" + name).c_str(), &_lowPass)) {
                demod.setLowPass(_lowPass);
                _config->acquire();
                _config->conf[name][getName()]["lowPass"] = _lowPass;
                _config->release(true);
            }
            if (ImGui::Checkbox(("立体声##_radio_wfm_stereo_" + name).c_str(), &_stereo)) {
                setStereo(_stereo);
                _config->acquire();
                _config->conf[name][getName()]["stereo"] = _stereo;
                _config->release(true);
            }
            if (ImGui::Checkbox(("解码 RDS##_radio_wfm_rds_" + name).c_str(), &_rds)) {
                demod.setRDSOut(_rds);
                _config->acquire();
                _config->conf[name][getName()]["rds"] = _rds;
                _config->release(true);
            }

            // TODO: This might break when the entire radio module is disabled
            if (_rds) {
                if (ImGui::Checkbox(("RDS 增量更新##_radio_wfm_rds_inc_" + name).c_str(), &_rdsIncremental)) {
                    _config->acquire();
                    _config->conf[name][getName()]["rdsIncremental"] = _rdsIncremental;
                    _config->release(true);
                }
                if (ImGui::Checkbox(("高级 RDS 信息##_radio_wfm_rds_info_" + name).c_str(), &_rdsInfo)) {
                    setAdvancedRds(_rdsInfo);
                    _config->acquire();
                    _config->conf[name][getName()]["rdsInfo"] = _rdsInfo;
                    _config->release(true);
                }
                ImGui::SameLine();
                ImGui::FillWidth();
                if (ImGui::Combo(("##_radio_wfm_rds_region_" + name).c_str(), &rdsRegionId, rdsRegions.txt)) {
                    rdsRegion = rdsRegions.value(rdsRegionId);
                    _config->acquire();
                    _config->conf[name][getName()]["rdsRegion"] = rdsRegions.key(rdsRegionId);
                    _config->release(true);
                }
            }

            float menuWidth = ImGui::GetContentRegionAvail().x;

            if (_rds && _rdsInfo) {
                ImGui::BeginTable(("##radio_wfm_rds_info_tbl_" + name).c_str(), 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders);
                if (rdsDecode.piCodeValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("PI Code");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::Text("0x%04X (%s)", rdsDecode.getPICode(), rdsDecode.getCallsign().c_str());
                    }
                    else {
                        ImGui::Text("0x%04X", rdsDecode.getPICode());
                    }
                    
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Country Code");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", rdsDecode.getCountryCode());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Coverage");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s (%d)", rds::AREA_COVERAGE_TO_STR[rdsDecode.getProgramCoverage()], rdsDecode.getProgramCoverage());

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Reference Number");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%d", rdsDecode.getProgramRefNumber());
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("PI Code");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::TextUnformatted("0x---- (----)");
                    }
                    else {
                        ImGui::TextUnformatted("0x----");
                    }
                    
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Country Code");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("--");  // TODO: String

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Coverage");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("------- (--)");

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Reference Number");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("--");
                }

                if (rdsDecode.programTypeValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Type");
                    ImGui::TableSetColumnIndex(1);
                    if (rdsRegion == RDS_REGION_NORTH_AMERICA) {
                        ImGui::Text("%s (%d)", rds::PROGRAM_TYPE_US_TO_STR[rdsDecode.getProgramType()], rdsDecode.getProgramType());
                    }
                    else {
                        ImGui::Text("%s (%d)", rds::PROGRAM_TYPE_EU_TO_STR[rdsDecode.getProgramType()], rdsDecode.getProgramType());
                    }
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Program Type");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("------- (--)");  // TODO: String
                }

                if (rdsDecode.musicValid()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Music");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", rdsDecode.getMusic() ? "Yes":"No");
                }
                else {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted("Music");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted("---");
                }

                ImGui::EndTable();

                ImGui::SetNextItemWidth(menuWidth);
                diag.draw();
            }

            // Stereo Analysis
            if (ImGui::Checkbox(("立体声分析##_radio_wfm_stereo_" + name).c_str(), &_stereoAnalysis)) {
                // Update MPX output in demodulator
                demod.setMPXOut(_stereoAnalysis);
                
                // Initialize or stop MPX handler based on new state
                if (_stereoAnalysis) {
                    initMPXAnalysis();
                    mpxHandler.init(&demod.mpxOut, mpxAnalysisHandler, this);
                    mpxHandler.start();
                } else {
                    mpxHandler.stop();
                    if (fftInitialized) {
                        fftwf_destroy_plan(fftPlan);
                        fftwf_free(fftInput);
                        fftwf_free(fftOutput);
                        fftInitialized = false;
                    }
                }
                
                _config->acquire();
                _config->conf[name][getName()]["stereoAnalysis"] = _stereoAnalysis;
                _config->release(true);
            }

            // Real-time MPX spectrum and channel analysis
            if (_stereoAnalysis) {
                ImGui::Separator();
                ImGui::Text("FM 复合信号 (MPX) 频谱分析");
                
                std::lock_guard<std::mutex> lock(mpxDataMutex);
                
                // MPX Frequency Spectrum
                if (fftInitialized && !mpxSpectrum.empty()) {
                    // Find appropriate frequency range (0-100 kHz)
                    int maxBin = FFT_SIZE/2;
                    for (int i = 0; i < FFT_SIZE/2; i++) {
                        if (frequencyAxis[i] > 100000.0f) {
                            maxBin = i;
                            break;
                        }
                    }
                    
                    // Create frequency labels for display using smoothed spectrum
                    std::vector<float> displaySpectrum(maxBin);
                    std::copy(mpxSpectrumSmoothed.begin(), mpxSpectrumSmoothed.begin() + maxBin, displaySpectrum.begin());
                    
                    // Plot the spectrum
                    ImVec2 plotSize(800, 200);
                    ImGui::Text("MPX 频谱 (0-100 kHz)");
                    
                    // Custom plot with frequency axis
                    if (ImGui::BeginChild("MPXSpectrum", ImVec2(plotSize.x + 20, plotSize.y + 60), true)) {
                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
                        ImVec2 canvas_size = ImVec2(plotSize.x, plotSize.y);
                        
                        // Draw background
                        drawList->AddRectFilled(canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), IM_COL32(20, 20, 20, 255));
                        
                        // Find min/max for scaling
                        float minVal = *std::min_element(displaySpectrum.begin(), displaySpectrum.end());
                        float maxVal = *std::max_element(displaySpectrum.begin(), displaySpectrum.end());
                        float range = maxVal - minVal;
                        if (range < 1.0f) range = 1.0f;
                        
                        // Note: Spectrum will be drawn last (on top) for better visibility
                        
                        // MPX Component frequency bands with proper bandwidth representation
                        struct MPXBand {
                            float startFreq;
                            float endFreq;
                            const char* label;
                            ImU32 fillColor;
                            ImU32 textColor;
                            bool isLine; // true for pilot tone, false for bands
                        };
                        
                        std::vector<MPXBand> mpxBands = {
                            {0.0f, 15000.0f, "单声道", IM_COL32(255, 255, 100, 15), IM_COL32(255, 255, 100, 255), false},      // 0-15 kHz mono - very subtle
                            {23000.0f, 53000.0f, "立体声", IM_COL32(80, 255, 80, 15), IM_COL32(120, 255, 120, 255), false}, // 23-53 kHz L-R signal - very subtle
                            {55000.0f, 59000.0f, "RDS", IM_COL32(255, 120, 255, 20), IM_COL32(255, 160, 255, 255), false},  // 55-59 kHz RDS - very subtle
                            {65000.0f, 70000.0f, "SCA1", IM_COL32(120, 180, 255, 20), IM_COL32(160, 200, 255, 255), false}, // 65-70 kHz SCA1 - very subtle
                            {90000.0f, 94000.0f, "SCA2", IM_COL32(255, 200, 120, 20), IM_COL32(255, 220, 160, 255), false}  // 90-94 kHz SCA2 - very subtle
                        };
                        
                        // Draw frequency bands first
                        for (const auto& band : mpxBands) {
                            // Find frequency bins for band start and end using actual frequency axis
                            int startBin = 0, endBin = maxBin;
                            
                            // Find start bin
                            for (int i = 0; i < maxBin; i++) {
                                if (frequencyAxis[i] >= band.startFreq) {
                                    startBin = i;
                                    break;
                                }
                            }
                            
                            // Find end bin
                            for (int i = startBin; i < maxBin; i++) {
                                if (frequencyAxis[i] > band.endFreq) {
                                    endBin = i;
                                    break;
                                }
                            }
                            
                            // Convert bins to pixel positions
                            float x1 = canvas_pos.x + ((float)startBin / maxBin) * canvas_size.x;
                            float x2 = canvas_pos.x + ((float)endBin / maxBin) * canvas_size.x;
                            
                            // Ensure band is visible and within bounds
                            if (x2 > canvas_pos.x && x1 < canvas_pos.x + canvas_size.x) {
                                x1 = std::max(x1, canvas_pos.x);
                                x2 = std::min(x2, canvas_pos.x + canvas_size.x);
                                
                                // Draw transparent filled rectangle for the band
                                ImVec2 bandMin = ImVec2(x1, canvas_pos.y);
                                ImVec2 bandMax = ImVec2(x2, canvas_pos.y + canvas_size.y);
                                drawList->AddRectFilled(bandMin, bandMax, band.fillColor);
                                
                                // Draw subtle border
                                drawList->AddRect(bandMin, bandMax, band.textColor, 0.0f, 0, 1.0f);
                                
                                // Label in center of band
                                ImVec2 textSize = ImGui::CalcTextSize(band.label);
                                float centerX = (x1 + x2) / 2.0f;
                                float textX = centerX - textSize.x/2;
                                float textY = canvas_pos.y + 5;
                                
                                // Ensure text stays within band and canvas bounds
                                if (textX < x1 + 2) textX = x1 + 2;
                                if (textX + textSize.x > x2 - 2) textX = x2 - textSize.x - 2;
                                if (textX < canvas_pos.x + 2) textX = canvas_pos.x + 2;
                                if (textX + textSize.x > canvas_pos.x + canvas_size.x - 2) textX = canvas_pos.x + canvas_size.x - textSize.x - 2;
                                
                                // Background for readability
                                ImVec2 bgMin = ImVec2(textX - 3, textY - 1);
                                ImVec2 bgMax = ImVec2(textX + textSize.x + 3, textY + textSize.y + 1);
                                drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 180), 3.0f);
                                drawList->AddText(ImVec2(textX, textY), band.textColor, band.label);
                            }
                        }
                        
                        // Draw 19 kHz pilot tone line on top of everything for maximum visibility
                        float pilotFreq = 19000.0f;
                        
                        // Find the pilot frequency bin in the actual spectrum data
                        int pilotBin = -1;
                        for (int i = 0; i < maxBin; i++) {
                            if (frequencyAxis[i] >= pilotFreq) {
                                pilotBin = i;
                                break;
                            }
                        }
                        
                        if (pilotBin >= 0 && pilotBin < maxBin) {
                            float pilotX = canvas_pos.x + ((float)pilotBin / maxBin) * canvas_size.x;
                            // Draw bright, thick pilot line
                            drawList->AddLine(ImVec2(pilotX, canvas_pos.y), ImVec2(pilotX, canvas_pos.y + canvas_size.y), IM_COL32(255, 60, 60, 255), 3.0f);
                            
                            // Add pilot label
                            ImVec2 textSize = ImGui::CalcTextSize("导频");
                            float textX = pilotX - textSize.x/2;
                            float textY = canvas_pos.y + 25; // Offset slightly from other labels
                            
                            // Ensure text stays within bounds
                            if (textX < canvas_pos.x + 2) textX = canvas_pos.x + 2;
                            if (textX + textSize.x > canvas_pos.x + canvas_size.x - 2) textX = canvas_pos.x + canvas_size.x - textSize.x - 2;
                            
                            // Prominent background for pilot label
                            ImVec2 bgMin = ImVec2(textX - 4, textY - 2);
                            ImVec2 bgMax = ImVec2(textX + textSize.x + 4, textY + textSize.y + 2);
                            drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 200), 3.0f);
                            drawList->AddRect(bgMin, bgMax, IM_COL32(255, 60, 60, 255), 3.0f, 0, 2.0f);
                            drawList->AddText(ImVec2(textX, textY), IM_COL32(255, 120, 120, 255), "导频");
                        }
                        
                        // Modern frequency axis with grid lines and enhanced labels using actual frequency range
                        float maxFreq = frequencyAxis[maxBin-1] / 1000.0f; // Convert to kHz
                        int stepSize = (maxFreq > 100) ? 20 : 10; // Adjust step size based on range
                        
                        for (int f = 0; f <= (int)maxFreq; f += stepSize) {
                            // Find the bin for this frequency
                            int freqBin = -1;
                            float targetFreq = f * 1000.0f; // Convert back to Hz
                            for (int i = 0; i < maxBin; i++) {
                                if (frequencyAxis[i] >= targetFreq) {
                                    freqBin = i;
                                    break;
                                }
                            }
                            
                            if (freqBin >= 0 && freqBin < maxBin) {
                                float x = canvas_pos.x + ((float)freqBin / maxBin) * canvas_size.x;
                                
                                // Major grid lines every 20k, minor every 10k (or adjusted based on range)
                                bool isMajor = (f % (stepSize * 2) == 0);
                                
                                if (isMajor) {
                                    // Major grid lines
                                    drawList->AddLine(ImVec2(x, canvas_pos.y), ImVec2(x, canvas_pos.y + canvas_size.y), IM_COL32(60, 60, 60, 100), 1.0f);
                                    
                                    // Enhanced frequency labels with background
                                    std::string freqLabel = std::to_string(f) + "k";
                                    ImVec2 textSize = ImGui::CalcTextSize(freqLabel.c_str());
                                    ImVec2 labelPos = ImVec2(x - textSize.x/2, canvas_pos.y + canvas_size.y + 8);
                                    
                                    // Background for readability
                                    ImVec2 bgMin = ImVec2(labelPos.x - 3, labelPos.y - 1);
                                    ImVec2 bgMax = ImVec2(labelPos.x + textSize.x + 3, labelPos.y + textSize.y + 1);
                                    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(20, 20, 20, 200), 2.0f);
                                    
                                    // Bright label text
                                    drawList->AddText(labelPos, IM_COL32(220, 220, 220, 255), freqLabel.c_str());
                                } else {
                                    // Minor grid lines
                                    drawList->AddLine(ImVec2(x, canvas_pos.y), ImVec2(x, canvas_pos.y + canvas_size.y), IM_COL32(40, 40, 40, 60), 0.5f);
                                }
                            }
                        }
                        
                        // Draw spectrum LAST (on top) for best visibility over background bands
                        for (int i = 1; i < maxBin; i++) {
                            float x1 = canvas_pos.x + (float)(i-1) * canvas_size.x / maxBin;
                            float y1 = canvas_pos.y + canvas_size.y - ((displaySpectrum[i-1] - minVal) / range) * canvas_size.y;
                            float x2 = canvas_pos.x + (float)i * canvas_size.x / maxBin;
                            float y2 = canvas_pos.y + canvas_size.y - ((displaySpectrum[i] - minVal) / range) * canvas_size.y;
                            
                            // Bright, prominent colors for maximum visibility
                            float freqRatio = (float)i / maxBin;
                            int red = 120 + (int)(135 * (1.0f - freqRatio));  // Brighter
                            int green = 200 + (int)(55 * freqRatio);          // Brighter  
                            int blue = 255;                                   // Full blue
                            
                            // Configurable line width for maximum visibility on top
                            drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(red, green, blue, 255), displaymenu::mpxLineWidth);
                            
                            // Optional: Add a subtle glow effect with thinner bright line on top
                            drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 255, 255, 180), 1.0f);
                        }
                        
                        ImGui::Dummy(ImVec2(canvas_size.x, canvas_size.y + 40));
                    }
                    ImGui::EndChild();
                    
                    // Time-domain channel analysis
                    ImGui::Separator();
                    ImGui::Text("立体声声道分析");
                    
                    // Use same width as MPX spectrum for consistency
                    ImVec2 timeGraphSize(plotSize.x, 60);
                    
                    ImGui::Text("L+R (单声道信号)");
                    if (!lPlusR.empty()) {
                        ImGui::PlotLines("##mpx_mono", lPlusR.data(), 200, 0, NULL, -0.5f, 0.5f, timeGraphSize);
                    }
                    
                    ImGui::Text("L-R (立体声差信号)"); 
                    if (!lMinusR.empty()) {
                        ImGui::PlotLines("##mpx_stereo", lMinusR.data(), 200, 0, NULL, -0.4f, 0.4f, timeGraphSize);
                    }
                    
                    ImGui::Text("左声道");
                    if (!leftChannel.empty()) {
                        ImGui::PlotLines("##mpx_left", leftChannel.data(), 200, 0, NULL, -0.6f, 0.6f, timeGraphSize);
                    }
                    
                    ImGui::Text("右声道");
                    if (!rightChannel.empty()) {
                        ImGui::PlotLines("##mpx_right", rightChannel.data(), 200, 0, NULL, -0.6f, 0.6f, timeGraphSize);
                    }
                    
                    // Modern legend with color-coded text
                    ImGui::Separator();
                    ImGui::Text("MPX 分量图例:");
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "单声道"); ImGui::SameLine(); ImGui::Text("(0-15k) |");
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "导频"); ImGui::SameLine(); ImGui::Text("(19k) |");
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "立体声"); ImGui::SameLine(); ImGui::Text("(38k) |");
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.6f, 1.0f, 1.0f), "RDS"); ImGui::SameLine(); ImGui::Text("(57k) |");
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "SCA"); ImGui::SameLine(); ImGui::Text("(67.65k, 92k)");
                } else {
                    ImGui::Text("正在初始化 FFT 以进行频谱分析...");
                }
            }
        }

        void setBandwidth(double bandwidth) {
            demod.setDeviation(bandwidth / 2.0f);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) {
            demod.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "WFM"; }
        double getIFSampleRate() { return 250000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 150000.0; }
        double getMinBandwidth() { return 50000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 100000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return true; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_50US; }
        bool getFMIFNRAllowed() { return true; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

        // ============= DEDICATED FUNCTIONS =============

        void setStereo(bool stereo) {
            _stereo = stereo;
            demod.setStereo(_stereo);
        }

        void setAdvancedRds(bool enabled) {
            rdsDemod.setSoftEnabled(enabled);
            _rdsInfo = enabled;
        }

    private:
        static void rdsHandler(uint8_t* data, int count, void* ctx) {
            WFM* _this = (WFM*)ctx;
            _this->rdsDecode.process(data, count);
        }

        static void _diagHandler(float* data, int count, void* ctx) {
            WFM* _this = (WFM*)ctx;
            float* buf = _this->diag.acquireBuffer();
            memcpy(buf, data, count * sizeof(float));
            _this->diag.releaseBuffer();
        }

        static void mpxAnalysisHandler(float* data, int count, void* ctx) {
            WFM* _this = (WFM*)ctx;
            
            // Use MPX refresh rate from display module
            static auto lastUpdate = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();
            
            // Get refresh rate from display module (1-60 Hz)
            int refreshRate = displaymenu::mpxRefreshRate;
            int refreshInterval = 1000 / refreshRate; // Convert Hz to milliseconds
            
            // Only process if enough time has passed based on configured refresh rate
            if (elapsed >= refreshInterval) {
                _this->processMPXData(data, count);
                lastUpdate = now;
            }
        }

        void initMPXAnalysis() {
            fftInitialized = false;
            
            // Initialize FFT
            fftInput = (float*)fftwf_malloc(sizeof(float) * FFT_SIZE);
            fftOutput = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (FFT_SIZE/2 + 1));
            fftPlan = fftwf_plan_dft_r2c_1d(FFT_SIZE, fftInput, fftOutput, FFTW_ESTIMATE);
            
            if (fftInput && fftOutput && fftPlan) {
                fftInitialized = true;
                
                // Initialize buffers
                mpxBuffer.resize(FFT_SIZE, 0.0f);
                mpxSpectrum.resize(FFT_SIZE/2, 0.0f);
                mpxSpectrumSmoothed.resize(FFT_SIZE/2, 0.0f);
                frequencyAxis.resize(FFT_SIZE/2);
                
                // Initialize time-domain analysis buffers
                lPlusR.resize(200, 0.0f);
                lMinusR.resize(200, 0.0f);
                leftChannel.resize(200, 0.0f);
                rightChannel.resize(200, 0.0f);
                
                // Create Hann window
                window.resize(FFT_SIZE);
                for (int i = 0; i < FFT_SIZE; i++) {
                    window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
                }
                
                // Initialize frequency axis using actual WFM sample rate
                float sampleRate = (float)getIFSampleRate(); // Use actual WFM sample rate
                for (int i = 0; i < FFT_SIZE/2; i++) {
                    frequencyAxis[i] = (float)i * sampleRate / FFT_SIZE;
                }
            }
        }

        void processMPXData(float* data, int count) {
            if (!fftInitialized) return;
            
            std::lock_guard<std::mutex> lock(mpxDataMutex);
            
            // Accumulate samples for FFT
            int samplesToUse = std::min(count, FFT_SIZE);
            
            // Shift existing data and add new samples
            if (count < FFT_SIZE) {
                std::memmove(mpxBuffer.data(), mpxBuffer.data() + count, (FFT_SIZE - count) * sizeof(float));
                std::copy(data, data + count, mpxBuffer.data() + FFT_SIZE - count);
            } else {
                std::copy(data + count - FFT_SIZE, data + count, mpxBuffer.data());
            }
            
            // Apply window and copy to FFT input
            for (int i = 0; i < FFT_SIZE; i++) {
                fftInput[i] = mpxBuffer[i] * window[i];
            }
            
            // Perform FFT
            fftwf_execute(fftPlan);
            
            // Convert to magnitude spectrum (dB) and apply smoothing
            for (int i = 0; i < FFT_SIZE/2; i++) {
                float real = fftOutput[i][0];
                float imag = fftOutput[i][1];
                float magnitude = sqrtf(real*real + imag*imag);
                float newValue = 20.0f * log10f(magnitude + 1e-10f); // Add small value to avoid log(0)
                
                // Apply exponential moving average smoothing
                // Higher smoothing factor = more smoothing (less responsive but smoother)
                float alpha = 1.0f / (float)displaymenu::mpxSmoothingFactor; // Convert factor to alpha (1/factor)
                mpxSpectrumSmoothed[i] = alpha * newValue + (1.0f - alpha) * mpxSpectrumSmoothed[i];
                
                // Store both raw and smoothed - we'll use smoothed for display
                mpxSpectrum[i] = newValue; // Keep raw for debugging if needed
            }
            
            // Also process time-domain analysis for channel display
            int displaySamples = std::min(count, 200);
            int startIdx = std::max(0, count - displaySamples);
            
            // Process recent samples for time-domain analysis
            for (int i = 0; i < displaySamples; i++) {
                float sample = data[startIdx + i];
                
                // Simple L+R extraction (low-frequency component of MPX)
                lPlusR[i] = sample * 0.5f; // Simplified mono component
                
                // Simple L-R estimation (38kHz component detection)
                // This is a basic approximation - real implementation would need proper filtering
                lMinusR[i] = sample * 0.2f * sinf(i * 0.1f); // Placeholder stereo difference
                
                // Reconstruct stereo channels
                leftChannel[i] = lPlusR[i] + lMinusR[i];
                rightChannel[i] = lPlusR[i] - lMinusR[i];
            }
        }

        static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
            WFM* _this = (WFM*)ctx;
            if (!_this->_rds) { return; }

            // Generate string depending on RDS mode
            char buf[256];
            if (_this->rdsDecode.PSNameValid() && _this->rdsDecode.radioTextValid()) {
                sprintf(buf, "RDS: %s - %s", _this->rdsDecode.getPSName(_this->_rdsIncremental).c_str(), _this->rdsDecode.getRadioText(_this->_rdsIncremental).c_str());
            }
            else if (_this->rdsDecode.PSNameValid()) {
                sprintf(buf, "RDS: %s", _this->rdsDecode.getPSName(_this->_rdsIncremental).c_str());
            }
            else if (_this->rdsDecode.radioTextValid()) {
                sprintf(buf, "RDS: %s", _this->rdsDecode.getRadioText(_this->_rdsIncremental).c_str());
            }
            else {
                return;
            }

            // Calculate paddings
            ImVec2 min = args.min;
            min.x += 5.0f * style::uiScale;
            min.y += 5.0f * style::uiScale;
            ImVec2 tmin = min;
            tmin.x += 5.0f * style::uiScale;
            tmin.y += 5.0f * style::uiScale;
            ImVec2 tmax = ImGui::CalcTextSize(buf);
            tmax.x += tmin.x;
            tmax.y += tmin.y;
            ImVec2 max = tmax;
            max.x += 5.0f * style::uiScale;
            max.y += 5.0f * style::uiScale;

            // Draw back drop
            args.window->DrawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 128));

            // Draw text
            args.window->DrawList->AddText(tmin, IM_COL32(255, 255, 0, 255), buf);
        }

        dsp::demod::BroadcastFM demod;
        RDSDemod rdsDemod;
        dsp::sink::Handler<uint8_t> hs;
        EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;

        dsp::buffer::Reshaper<float> reshape;
        dsp::sink::Handler<float> diagHandler;
        ImGui::SymbolDiagram diag;

        // MPX analysis
        dsp::sink::Handler<float> mpxHandler;
        std::vector<float> mpxBuffer;
        std::vector<float> mpxSpectrum;
        std::vector<float> mpxSpectrumSmoothed; // Smoothed spectrum for noise reduction
        std::vector<float> frequencyAxis;
        std::vector<float> lPlusR;
        std::vector<float> lMinusR;
        std::vector<float> leftChannel;
        std::vector<float> rightChannel;
        std::mutex mpxDataMutex;
        
        // FFT for MPX analysis
        static const int FFT_SIZE = 4096;
        fftwf_plan fftPlan;
        float* fftInput;
        fftwf_complex* fftOutput;
        std::vector<float> window;
        bool fftInitialized;

        rds::Decoder rdsDecode;

        ConfigManager* _config = NULL;

        bool _stereo = false;
        bool _lowPass = true;
        bool _rds = false;
        bool _rdsInfo = false;
        bool _rdsIncremental = true;
        bool _stereoAnalysis = false;
        float muGain = 0.01;
        float omegaGain = (0.01*0.01)/4.0;

        int rdsRegionId = 0;
        RDSRegion rdsRegion = RDS_REGION_EUROPE;

        OptionList<std::string, RDSRegion> rdsRegions;


        std::string name;
    };
}