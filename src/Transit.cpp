#include "plugin.hpp"
#include "digital.hpp"
#include <plugin.hpp>
#include <random>

namespace StoermelderPackOne {
namespace Transit {

enum class SLOTCVMODE {
	TRIG_FWD = 2,
	TRIG_REV = 4,
	TRIG_PINGPONG = 5,
	TRIG_RANDOM = 6,
	VOLT = 0,
	C4 = 1,
	ARM = 3
};

enum class OUTMODE {
	ENV = 0,
	GATE = 1,
	EOC = 2
};

template < int NUM_PRESETS >
struct TransitModule : Module {
	enum ParamIds {
		PARAM_RW,
		PARAM_FADE,
		PARAM_SHAPE,
		ENUMS(PARAM_PRESET, NUM_PRESETS),
		NUM_PARAMS
	};
	enum InputIds {
		INPUT_SLOT,
		INPUT_RESET,
		INPUT_FADE,
		NUM_INPUTS
	};
	enum OutputIds {
		OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(LIGHT_PRESET, NUM_PRESETS * 3),
		NUM_LIGHTS
	};

	/** [Stored to JSON] */
	int panelTheme = 0;

	/** [Stored to JSON] */
	bool presetSlotUsed[NUM_PRESETS];
	/** [Stored to JSON] */
	std::vector<float> presetSlot[NUM_PRESETS];
	/** [Stored to JSON] */
	int preset;
	/** [Stored to JSON] */
	int presetCount;

	int presetNext;

	/** Holds the last values on transitions */
	std::vector<float> presetOld;

	/** [Stored to JSON] mode for SEQ CV input */
	SLOTCVMODE slotCvMode = SLOTCVMODE::TRIG_FWD;
	int slotCvModeDir = 1;

	/** [Stored to JSON] */
	OUTMODE outMode;
	bool outEocArm;
	dsp::PulseGenerator outEocPulseGenerator;

	/** [Stored to JSON] */
	bool mappingIndicatorHidden = false;
	/** [Stored to JSON] */
	int presetProcessDivision;
	dsp::ClockDivider presetProcessDivider;

	std::default_random_engine randGen{(uint16_t)std::chrono::system_clock::now().time_since_epoch().count()};
	std::uniform_int_distribution<int> randDist;
	bool inChange = false;

	/** [Stored to JSON] */
	std::vector<ParamHandle*> sourceHandles;

	LongPressButton typeButtons[NUM_PRESETS];
	dsp::SchmittTrigger slotTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::Timer resetTimer;

	StoermelderShapedSlewLimiter slewLimiter;
	dsp::ClockDivider handleDivider;
	dsp::ClockDivider lightDivider;
	dsp::ClockDivider buttonDivider;

	int sampleRate;

	TransitModule() {
		panelTheme = pluginSettings.panelThemeDefault;
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(PARAM_RW, 0, 1, 0, "Read/write mode");
		for (int i = 0; i < NUM_PRESETS; i++) {
			configParam<TriggerParamQuantity>(PARAM_PRESET + i, 0, 1, 0, string::f("Slot #%d", i + 1));
			typeButtons[i].param = &params[PARAM_PRESET + i];
			presetSlotUsed[i] = false;
		}
		configParam(PARAM_FADE, 0.f, 1.f, 0.5f, "Fade");
		configParam(PARAM_SHAPE, -1.f, 1.f, 0.f, "Shape");

		handleDivider.setDivision(4096);
		lightDivider.setDivision(512);
		buttonDivider.setDivision(4);
		onReset();
	}

	~TransitModule() {
		for (ParamHandle* sourceHandle : sourceHandles) {
			APP->engine->removeParamHandle(sourceHandle);
			delete sourceHandle;
		}
	}

	void onReset() override {
		inChange = true;
		for (ParamHandle* sourceHandle : sourceHandles) {
			APP->engine->removeParamHandle(sourceHandle);
			delete sourceHandle;
		}
		sourceHandles.clear();
		inChange = false;

		for (int i = 0; i < NUM_PRESETS; i++) {
			presetSlotUsed[i] = false;
			presetSlot[i].clear();
		}

		preset = -1;
		presetCount = NUM_PRESETS;
		presetNext = -1;
		slewLimiter.reset(10.f);

		outMode = OUTMODE::ENV;

		randDist = std::uniform_int_distribution<int>(0, presetCount - 1);
		mappingIndicatorHidden = false;
		presetProcessDivision = 8;
		presetProcessDivider.setDivision(presetProcessDivision);
		presetProcessDivider.reset();
		
		Module::onReset();
	}

	void process(const ProcessArgs& args) override {
		if (inChange) return;
		sampleRate = args.sampleRate;

		if (handleDivider.process()) {
			for (size_t i = 0; i < sourceHandles.size(); i++) {
				ParamHandle* sourceHandle = sourceHandles[i];
				sourceHandle->color = mappingIndicatorHidden ? color::BLACK_TRANSPARENT : nvgRGB(0x40, 0xff, 0xff);
			}
		}

		// Read mode
		if (params[PARAM_RW].getValue() == 0.f) {
			// RESET input
			if (slotCvMode == SLOTCVMODE::TRIG_FWD || slotCvMode == SLOTCVMODE::TRIG_REV || slotCvMode == SLOTCVMODE::TRIG_PINGPONG) {
				if (inputs[INPUT_RESET].isConnected() && resetTrigger.process(inputs[INPUT_RESET].getVoltage())) {
					resetTimer.reset();
					presetLoad(0);
				}
			}

			// SLOT input
			if (inputs[INPUT_SLOT].isConnected() && resetTimer.process(args.sampleTime) >= 1e-3f) {
				switch (slotCvMode) {
					case SLOTCVMODE::VOLT:
						presetLoad(std::floor(rescale(inputs[INPUT_SLOT].getVoltage(), 0.f, 10.f, 0, presetCount)));
						break;
					case SLOTCVMODE::C4:
						presetLoad(std::round(clamp(inputs[INPUT_SLOT].getVoltage() * 12.f, 0.f, NUM_PRESETS - 1.f)));
						break;
					case SLOTCVMODE::TRIG_FWD:
						if (slotTrigger.process(inputs[INPUT_SLOT].getVoltage()))
							presetLoad((preset + 1) % presetCount);
						break;
					case SLOTCVMODE::TRIG_REV:
						if (slotTrigger.process(inputs[INPUT_SLOT].getVoltage()))
							presetLoad((preset - 1 + presetCount) % presetCount);
						break;
					case SLOTCVMODE::TRIG_PINGPONG:
						if (slotTrigger.process(inputs[INPUT_SLOT].getVoltage())) {
							int n = preset + slotCvModeDir;
							if (n == presetCount - 1) 
								slotCvModeDir = -1;
							if (n == 0) 
								slotCvModeDir = 1;
							presetLoad(n);
						}
						break;
					case SLOTCVMODE::TRIG_RANDOM:
						if (slotTrigger.process(inputs[INPUT_SLOT].getVoltage()))
							presetLoad(randDist(randGen));
						break;
					case SLOTCVMODE::ARM:
						if (slotTrigger.process(inputs[INPUT_SLOT].getVoltage()))
							presetLoad(presetNext);
						break;
				}
			}

			// Buttons
			if (buttonDivider.process()) {
				float sampleTime = args.sampleTime * buttonDivider.division;
				for (int i = 0; i < NUM_PRESETS; i++) {
					switch (typeButtons[i].process(sampleTime)) {
						default:
						case LongPressButton::NO_PRESS:
							break;
						case LongPressButton::SHORT_PRESS:
							presetLoad(i, slotCvMode == SLOTCVMODE::ARM, true); break;
						case LongPressButton::LONG_PRESS:
							presetSetCount(i + 1); break;
					}
				}
			}
		}
		// Write mode
		else {
			if (buttonDivider.process()) {
				float sampleTime = args.sampleTime * buttonDivider.division;
				for (int i = 0; i < NUM_PRESETS; i++) {
					switch (typeButtons[i].process(sampleTime)) {
						default:
						case LongPressButton::NO_PRESS:
							break;
						case LongPressButton::SHORT_PRESS:
							presetSave(i); break;
						case LongPressButton::LONG_PRESS:
							presetClear(i); break;
					}
				}
			}
		}

		presetProcess(args.sampleTime);

		// Set channel lights infrequently
		if (lightDivider.process()) {
			float s = args.sampleTime * lightDivider.getDivision();
			for (int i = 0; i < NUM_PRESETS; i++) {
				if (params[PARAM_RW].getValue() == 0.f) {
					lights[LIGHT_PRESET + i * 3 + 0].setBrightness(presetNext == i ? 1.f : 0.f);
					lights[LIGHT_PRESET + i * 3 + 1].setSmoothBrightness(preset != i && presetCount > i ? (presetSlotUsed[i] ? 1.f : 0.2f) : 0.f, s);
					lights[LIGHT_PRESET + i * 3 + 2].setSmoothBrightness(preset == i ? 1.f : 0.f, s);
				}
				else {
					lights[LIGHT_PRESET + i * 3 + 0].setBrightness(presetSlotUsed[i] ? 1.f : 0.f);
					lights[LIGHT_PRESET + i * 3 + 1].setBrightness(0.f);
					lights[LIGHT_PRESET + i * 3 + 2].setBrightness(0.f);
				}
			}
		}
	}

	ParamQuantity* getParamQuantity(ParamHandle* handle) {
		if (handle->moduleId < 0)
			return NULL;
		// Get Module
		Module* module = handle->module;
		if (!module)
			return NULL;
		// Get ParamQuantity
		int paramId = handle->paramId;
		ParamQuantity* paramQuantity = module->paramQuantities[paramId];
		if (!paramQuantity)
			return NULL;
		return paramQuantity;
	}

	void bindModule() {
		Expander* exp = &leftExpander;
		if (exp->moduleId < 0) return;

		Module* m = exp->module;
		for (size_t i = 0; i < m->params.size(); i++) {
			bindParameter(m->id, i);
		}
	}

	void bindParameter(int moduleId, int paramId) {
		ParamHandle* sourceHandle = new ParamHandle;
		sourceHandle->text = "stoermelder TRANSIT";
		APP->engine->addParamHandle(sourceHandle);
		APP->engine->updateParamHandle(sourceHandle, moduleId, paramId, true);
		inChange = true;
		sourceHandles.push_back(sourceHandle);
		inChange = false;

		ParamQuantity* pq = getParamQuantity(sourceHandle);
		if (pq) {
			float v = pq->getValue();
			for (size_t i = 0; i < NUM_PRESETS; i++) {
				if (!presetSlotUsed[i]) continue;
				presetSlot[i].push_back(v);
			}
		}
	}

	void presetLoad(int p, bool isNext = false, bool force = false) {
		if (p < 0 || p >= presetCount)
			return;

		if (!isNext) {
			if (p != preset || force) {
				preset = p;
				presetNext = -1;
				if (!presetSlotUsed[p]) return;
				slewLimiter.reset();
				outEocArm = true;
				presetOld.clear();
				for (size_t i = 0; i < sourceHandles.size(); i++) {
					ParamQuantity* pq = getParamQuantity(sourceHandles[i]);
					presetOld.push_back(pq ? pq->getValue() : 0.f);
				}
			}
		}
		else {
			if (!presetSlotUsed[p]) return;
			presetNext = p;
		}
	}

	void presetProcess(float sampleTime) {
		if (presetProcessDivider.process()) {
			if (preset == -1) return;
			float deltaTime = sampleTime * presetProcessDivision;

			float fade = inputs[INPUT_FADE].getVoltage() / 10.f + params[PARAM_FADE].getValue();
			slewLimiter.setRise(fade);
			float shape = params[PARAM_SHAPE].getValue();
			slewLimiter.setShape(shape);
			float s = slewLimiter.process(10.f, deltaTime);

			if (s == 10.f && outEocArm) {
				outEocPulseGenerator.trigger();
				outEocArm = false;
			}

			switch (outMode) {
				case OUTMODE::ENV:
					outputs[OUTPUT].setVoltage(s == 10.f ? 0.f : s);
					break;
				case OUTMODE::GATE:
					outputs[OUTPUT].setVoltage(s != 10.f ? 10.f : 0.f);
					break;
				case OUTMODE::EOC:
					outputs[OUTPUT].setVoltage(outEocPulseGenerator.process(deltaTime) ? 10.f : 0.f);
					break;
			}

			if (s == 10.f) return;
			s /= 10.f;

			for (size_t i = 0; i < sourceHandles.size(); i++) {
				ParamQuantity* pq = getParamQuantity(sourceHandles[i]);
				if (!pq) continue;
				if (presetOld.size() <= i) return;
				float oldValue = presetOld[i];
				if (presetSlot[preset].size() <= i) return;
				float newValue = presetSlot[preset][i];
				float v = crossfade(oldValue, newValue, s);
				if (s > (1.f - 5e-3f) && std::abs(std::round(v) - v) < 5e-3f) v = std::round(v);
				pq->setValue(v);
			}
		}
		presetProcessDivider.setDivision(presetProcessDivision);
	}

	void presetSave(int p) {
		presetSlotUsed[p] = true;
		presetSlot[p].clear();
		for (size_t i = 0; i < sourceHandles.size(); i++) {
			ParamQuantity* pq = getParamQuantity(sourceHandles[i]);
			if (!pq) continue;
			float v = pq->getValue();
			presetSlot[p].push_back(v);
		}
	}

	void presetClear(int p) {
		presetSlotUsed[p] = false;
		presetSlot[p].clear();
		if (preset == p) preset = -1;
	}

	void presetSetCount(int p) {
		if (preset >= p) preset = 0;
		presetCount = p;
		presetNext = -1;
		randDist = std::uniform_int_distribution<int>(0, presetCount - 1);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "panelTheme", json_integer(panelTheme));
		json_object_set_new(rootJ, "mappingIndicatorHidden", json_boolean(mappingIndicatorHidden));
		json_object_set_new(rootJ, "presetProcessDivision", json_integer(presetProcessDivision));

		json_object_set_new(rootJ, "slotCvMode", json_integer((int)slotCvMode));
		json_object_set_new(rootJ, "outMode", json_integer((int)outMode));
		json_object_set_new(rootJ, "preset", json_integer(preset));
		json_object_set_new(rootJ, "presetCount", json_integer(presetCount));

		json_t* sourceMapsJ = json_array();
		for (size_t i = 0; i < sourceHandles.size(); i++) {
			json_t* sourceMapJ = json_object();
			json_object_set_new(sourceMapJ, "moduleId", json_integer(sourceHandles[i]->moduleId));
			json_object_set_new(sourceMapJ, "paramId", json_integer(sourceHandles[i]->paramId));
			json_array_append_new(sourceMapsJ, sourceMapJ);
		}
		json_object_set_new(rootJ, "sourceMaps", sourceMapsJ);

		json_t* presetsJ = json_array();
		for (int i = 0; i < NUM_PRESETS; i++) {
			json_t* presetJ = json_object();
			json_object_set_new(presetJ, "slotUsed", json_boolean(presetSlotUsed[i]));
			if (presetSlotUsed[i]) {
				json_t* slotJ = json_array();
				for (size_t j = 0; j < presetSlot[i].size(); j++) {
					json_t* vJ = json_real(presetSlot[i][j]);
					json_array_append_new(slotJ, vJ);
				}
				json_object_set(presetJ, "slot", slotJ);
			}
			json_array_append_new(presetsJ, presetJ);
		}
		json_object_set_new(rootJ, "presets", presetsJ);

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		panelTheme = json_integer_value(json_object_get(rootJ, "panelTheme"));
		mappingIndicatorHidden = json_boolean_value(json_object_get(rootJ, "mappingIndicatorHidden"));
		presetProcessDivision = json_integer_value(json_object_get(rootJ, "presetProcessDivision"));

		slotCvMode = (SLOTCVMODE)json_integer_value(json_object_get(rootJ, "slotCvMode"));
		outMode = (OUTMODE)json_integer_value(json_object_get(rootJ, "outMode"));
		preset = json_integer_value(json_object_get(rootJ, "preset"));
		presetCount = json_integer_value(json_object_get(rootJ, "presetCount"));

		// Hack for preventing duplicating this module
		if (APP->engine->getModule(id) != NULL) return;

		inChange = true;
		json_t* sourceMapsJ = json_object_get(rootJ, "sourceMaps");
		if (sourceMapsJ) {
			json_t* sourceMapJ;
			size_t sourceMapIndex;
			json_array_foreach(sourceMapsJ, sourceMapIndex, sourceMapJ) {
				json_t* moduleIdJ = json_object_get(sourceMapJ, "moduleId");
				json_t* paramIdJ = json_object_get(sourceMapJ, "paramId");

				ParamHandle* sourceHandle = new ParamHandle;
				sourceHandle->text = "stoermelder TRANSIT";
				APP->engine->addParamHandle(sourceHandle);
				APP->engine->updateParamHandle(sourceHandle, json_integer_value(moduleIdJ), json_integer_value(paramIdJ), false);
				sourceHandles.push_back(sourceHandle);
			}
		}
		inChange = false;

		json_t* presetsJ = json_object_get(rootJ, "presets");
		json_t* presetJ;
		size_t presetIndex;
		json_array_foreach(presetsJ, presetIndex, presetJ) {
			presetSlotUsed[presetIndex] = json_boolean_value(json_object_get(presetJ, "slotUsed"));
			presetSlot[presetIndex].clear();
			if (presetSlotUsed[presetIndex]) {
				json_t* slotJ = json_object_get(presetJ, "slot");
				json_t* vJ;
				size_t j;
				json_array_foreach(slotJ, j, vJ) {
					float v = json_real_value(vJ);
					presetSlot[presetIndex].push_back(v);
				}
			}
		}

		if (preset >= presetCount) {
			preset = 0;
		}
	}
};

template < int NUM_PRESETS >
struct TransitWidget : ThemedModuleWidget<TransitModule<NUM_PRESETS>> {
	typedef TransitWidget<NUM_PRESETS> WIDGET;
	typedef ThemedModuleWidget<TransitModule<NUM_PRESETS>> BASE;
	typedef TransitModule<NUM_PRESETS> MODULE;
	
	bool learnParam = false;

	TransitWidget(MODULE* module)
		: ThemedModuleWidget<MODULE>(module, "Transit") {
		BASE::setModule(module);

		BASE::addChild(createWidget<StoermelderBlackScrew>(Vec(RACK_GRID_WIDTH, 0)));
		BASE::addChild(createWidget<StoermelderBlackScrew>(Vec(BASE::box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		for (size_t i = 0; i < NUM_PRESETS; i++) {
			float o = i * (288.7f / (NUM_PRESETS - 1));
			BASE::addParam(createParamCentered<LEDButton>(Vec(17.1f, 45.4f + o), module, MODULE::PARAM_PRESET + i));
			BASE::addChild(createLightCentered<LargeLight<RedGreenBlueLight>>(Vec(17.1f, 45.4f + o), module, MODULE::LIGHT_PRESET + i * 3));
		}

		BASE::addInput(createInputCentered<StoermelderPort>(Vec(52.6f, 58.9f), module, MODULE::INPUT_SLOT));
		BASE::addInput(createInputCentered<StoermelderPort>(Vec(52.6f, 94.2f), module, MODULE::INPUT_RESET));

		BASE::addParam(createParamCentered<LEDSliderBlue>(Vec(52.6f, 166.7f), module, MODULE::PARAM_FADE));
		BASE::addInput(createInputCentered<StoermelderPort>(Vec(52.6f, 221.4f), module, MODULE::INPUT_FADE));

		BASE::addParam(createParamCentered<StoermelderTrimpot>(Vec(52.6f, 255.8f), module, MODULE::PARAM_SHAPE));
		BASE::addOutput(createOutputCentered<StoermelderPort>(Vec(52.6f, 300.3f), module, MODULE::OUTPUT));

		BASE::addParam(createParamCentered<CKSSH>(Vec(52.6f, 336.2f), module, MODULE::PARAM_RW));
	}

	void appendContextMenu(Menu* menu) override {
		ThemedModuleWidget<MODULE>::appendContextMenu(menu);
		MODULE* module = dynamic_cast<MODULE*>(this->module);
		assert(module);

		struct MappingIndicatorHiddenItem : MenuItem {
			MODULE* module;
			void onAction(const event::Action& e) override {
				module->mappingIndicatorHidden ^= true;
			}
			void step() override {
				rightText = module->mappingIndicatorHidden ? "✔" : "";
				MenuItem::step();
			}
		};

		struct PrecisionMenuItem : MenuItem {
			struct PrecisionItem : MenuItem {
				MODULE* module;
				int division;
				std::string text;
				void onAction(const event::Action& e) override {
					module->presetProcessDivision = division;
				}
				void step() override {
					MenuItem::text = string::f("%s (%i Hz)", text.c_str(), module->sampleRate / division);
					rightText = module->presetProcessDivision == division ? "✔" : "";
					MenuItem::step();
				}
			};

			MODULE* module;
			PrecisionMenuItem() {
				rightText = RIGHT_ARROW;
			}

			Menu* createChildMenu() override {
				Menu* menu = new Menu;
				menu->addChild(construct<PrecisionItem>(&PrecisionItem::text, "Audio rate", &PrecisionItem::module, module, &PrecisionItem::division, 1));
				menu->addChild(construct<PrecisionItem>(&PrecisionItem::text, "Lower CPU", &PrecisionItem::module, module, &PrecisionItem::division, 8));
				menu->addChild(construct<PrecisionItem>(&PrecisionItem::text, "Lowest CPU", &PrecisionItem::module, module, &PrecisionItem::division, 64));
				return menu;
			}
		};

		struct SlotCvModeMenuItem : MenuItem {
			struct SlotCvModeItem : MenuItem {
				MODULE* module;
				SLOTCVMODE slotCvMode;
				void onAction(const event::Action& e) override {
					module->slotCvMode = slotCvMode;
				}
				void step() override {
					rightText = module->slotCvMode == slotCvMode ? "✔" : "";
					MenuItem::step();
				}
			};

			MODULE* module;
			SlotCvModeMenuItem() {
				rightText = RIGHT_ARROW;
			}

			Menu* createChildMenu() override {
				Menu* menu = new Menu;
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "Trigger forward", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::TRIG_FWD));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "Trigger reverse", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::TRIG_REV));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "Trigger pingpong", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::TRIG_PINGPONG));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "Trigger random", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::TRIG_RANDOM));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "0..10V", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::VOLT));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "C4", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::C4));
				menu->addChild(construct<SlotCvModeItem>(&MenuItem::text, "Arm", &SlotCvModeItem::module, module, &SlotCvModeItem::slotCvMode, SLOTCVMODE::ARM));
				return menu;
			}
		};

		struct OutModeMenuItem : MenuItem {
			struct OutModeItem : MenuItem {
				MODULE* module;
				OUTMODE outMode;
				void onAction(const event::Action& e) override {
					module->outMode = outMode;
				}
				void step() override {
					rightText = module->outMode == outMode ? "✔" : "";
					MenuItem::step();
				}
			};

			MODULE* module;
			OutModeMenuItem() {
				rightText = RIGHT_ARROW;
			}

			Menu* createChildMenu() override {
				Menu* menu = new Menu;
				menu->addChild(construct<OutModeItem>(&MenuItem::text, "Envelope", &OutModeItem::module, module, &OutModeItem::outMode, OUTMODE::ENV));
				menu->addChild(construct<OutModeItem>(&MenuItem::text, "Gate", &OutModeItem::module, module, &OutModeItem::outMode, OUTMODE::GATE));
				menu->addChild(construct<OutModeItem>(&MenuItem::text, "EOC", &OutModeItem::module, module, &OutModeItem::outMode, OUTMODE::EOC));
				return menu;
			}
		};

		struct BindModuleItem : MenuItem {
			MODULE* module;
			void onAction(const event::Action& e) override {
				module->bindModule();
			}
		};

		struct BindParameterItem : MenuItem {
			WIDGET* widget;
			void onAction(const event::Action& e) override {
				widget->learnParam ^= true;
				APP->scene->rack->touchedParam = NULL;
				APP->event->setSelected(widget);
			}
			void step() override {
				rightText = widget->learnParam ? "Active" : "";
				MenuItem::step();
			}
		};

		menu->addChild(new MenuSeparator());
		menu->addChild(construct<MappingIndicatorHiddenItem>(&MenuItem::text, "Hide mapping indicators", &MappingIndicatorHiddenItem::module, module));
		menu->addChild(construct<PrecisionMenuItem>(&MenuItem::text, "Precision", &PrecisionMenuItem::module, module));
		menu->addChild(new MenuSeparator());
		menu->addChild(construct<SlotCvModeMenuItem>(&MenuItem::text, "SLOT-port", &SlotCvModeMenuItem::module, module));
		menu->addChild(construct<OutModeMenuItem>(&MenuItem::text, "OUT-port", &OutModeMenuItem::module, module));
		menu->addChild(new MenuSeparator());
		menu->addChild(construct<BindModuleItem>(&MenuItem::text, "Bind module (left)", &BindModuleItem::module, module));
		menu->addChild(construct<BindParameterItem>(&MenuItem::text, "Bind parameter", &BindParameterItem::widget, this));
	}

	void onDeselect(const event::Deselect& e) override {
		if (!learnParam) return;
		MODULE* module = dynamic_cast<MODULE*>(this->module);

		// Check if a ParamWidget was touched, unstable API
		ParamWidget* touchedParam = APP->scene->rack->touchedParam;
		if (touchedParam && touchedParam->paramQuantity->module != module) {
			APP->scene->rack->touchedParam = NULL;
			int moduleId = touchedParam->paramQuantity->module->id;
			int paramId = touchedParam->paramQuantity->paramId;
			module->bindParameter(moduleId, paramId);
			learnParam = false;
		} 
	}
};

} // namespace Transit
} // namespace StoermelderPackOne

Model* modelTransit = createModel<StoermelderPackOne::Transit::TransitModule<14>, StoermelderPackOne::Transit::TransitWidget<14>>("Transit");