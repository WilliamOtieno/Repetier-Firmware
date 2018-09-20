#include "../../Repetier.h"

#ifndef NUM_TOOLS
#error "Number of tools NUM_TOOLS not defined."
#endif

#if NUM_TOOLS < 1
#error "You need at least one tool to compile firmware"
#endif

HeatManager* heatedBeds[] = HEATED_BED_LIST;

fast8_t Tool::activeToolId = 255;
Tool* Tool::activeTool = nullptr;
Tool* const Tool::tools[NUM_TOOLS] = TOOLS;

void Tool::unselectTool() {
    if (activeTool == nullptr) {
        return;
    }
    Motion1::setToolOffset(0, 0, 0);
    activeTool->deactivate();
    PrinterType::deactivatedTool(activeToolId);
    activeTool = nullptr;
    activeToolId = 255;
}

void Tool::selectTool(fast8_t id, bool force) {
    if (Motion1::dittoMode) { // in ditto mode 0 is always active!
        id = 0;
    }
    if (id < 0 || id >= NUM_TOOLS) {
        Com::printErrorF(PSTR("Illegal tool number selected:"));
        Com::print((int)id);
        Com::println();
        if (activeTool != nullptr) {
            Com::printFLN(PSTR("SelectTool:"), static_cast<int>(activeToolId));
        }
        return;
    }
    if (!force && activeTool != nullptr && activeToolId == id) {
        activeTool->updateDerived(); // reset values
        return;                      // already selected
    }
    Com::printFLN(PSTR("SelectTool:"), static_cast<int>(id));
    float zOffset = -Motion1::toolOffset[Z_AXIS]; // opposite sign to extruder offset!
#if RAISE_Z_ON_TOOLCHANGE > 0
    float lastZ = Motion1::currentPosition[X_AXIS];
    Motion1::setTmpPositionXYZ(lastZ + RAISE_Z_ON_TOOLCHANGE, IGNORE_COORDINATE, IGNORE_COORDINATE);
    Motion1::moveByOfficial(Motion1::tmpPosition, Z_SPEED, false);
    Motion1::waitForEndOfMoves();
#endif
    float zOffsetNew = tools[id]->getOffsetZ();
    if (activeTool != nullptr) {
        if (zOffsetNew < zOffset) { // will hit bed, activate early
            Motion1::setToolOffset(-activeTool->getOffsetX(), -activeTool->getOffsetY(), -zOffsetNew);
        }
        activeTool->deactivate();
        PrinterType::deactivatedTool(activeToolId);
    }
    Tool::activeToolId = id;
    Tool::activeTool = tools[id];
    Motion1::advanceK = 0; // Gets activated by tool activation if supported!
    Motion2::advanceSteps = 0;
    activeTool->activate();
    PrinterType::activatedTool(activeToolId);
    Motion1::setToolOffset(-activeTool->getOffsetX(), -activeTool->getOffsetY(), -activeTool->getOffsetZ());
#if RAISE_Z_ON_TOOLCHANGE > 0
    float lastZ = Motion1::currentPosition[X_AXIS];
    Motion1::setTmpPositionXYZ(lastZ, IGNORE_COORDINATE, IGNORE_COORDINATE);
    Motion1::moveByOfficial(Motion1::tmpPosition, Z_SPEED, false);
    Motion1::waitForEndOfMoves();
#endif
}

void Tool::resetBase(float offX, float offY, float offZ) {
    offsetX = offX;
    offsetY = offY;
    offsetZ = offZ;
}

/* Tool* Tool::getTool(fast8_t id) {
    if (id < 0 || id >= NUM_TOOLS) {
        return nullptr;
    }
    return tools[id];
} */

void Tool::initTools() {
    for (fast8_t i = 0; i < NUM_TOOLS; i++) {
        tools[i]->setToolId(i);
        tools[i]->init();
    }
}

void Tool::disableMotors() {
    for (fast8_t i = 0; i < NUM_TOOLS; i++) {
        tools[i]->disableMotor();
    }
}

void Tool::eepromHandleTools() {
    for (fast8_t i = 0; i < NUM_TOOLS; i++) {
        EEPROM::handlePrefix(PSTR("Tool"), i + 1);
        tools[i]->eepromHandle();
    }
    EEPROM::removePrefix();
}

void Tool::eepromHandle() {
    EEPROM::handleFloat(eepromStart, PSTR("X Offset [mm]"), 3, offsetX);
    EEPROM::handleFloat(eepromStart + 4, PSTR("Y Offset [mm]"), 3, offsetY);
    EEPROM::handleFloat(eepromStart + 8, PSTR("Z Offset [mm]"), 3, offsetZ);
}

void Tool::updateDerivedTools() {
    if (activeTool != nullptr) {
        activeTool->updateDerived();
    }
}

void ToolExtruder::reset(float offx, float offy, float offz, float diameter, float resolution, float yank, float maxSpeed, float acceleration, float advance) {
    resetBase(offx, offy, offz);
    this->diameter = diameter;
    stepsPerMM = resolution;
    this->yank = yank;
    this->acceleration = acceleration;
    this->maxSpeed = maxSpeed;
    this->advance = advance;
}

/// Called when the tool gets activated.
void ToolExtruder::activate() {
    Motion1::setMotorForAxis(stepper, E_AXIS);
    Motion1::maxYank[E_AXIS] = yank;
    Motion1::resolution[E_AXIS] = stepsPerMM;
    Motion1::currentPosition[E_AXIS] = Motion1::currentPositionTransformed[E_AXIS] = 0.0f;
    Motion1::advanceK = advance;
    GCode::executeFString(startScript);
    Motion1::waitForEndOfMoves();
}

/// Gets called when the tool gets disabled.
void ToolExtruder::deactivate() {
    GCode::executeFString(endScript);
    Motion1::setMotorForAxis(nullptr, E_AXIS);
}

/// Called on kill/emergency to disable the tool
void ToolExtruder::shutdown() {
    heater->setTargetTemperature(0);
    stepper->disable();
}

void ToolExtruder::init() {
    setEepromStart(EEPROM::reserve(EEPROM_SIGNATURE_EXTRUDER, 1, Tool::eepromSize() + 6 * 4));
}

void ToolExtruder::eepromHandle() {
    Tool::eepromHandle();
    uint pos = getEepromStart() + Tool::eepromSize();
    EEPROM::handleFloat(pos, PSTR("Steps per mm [steps/mm]"), 2, stepsPerMM);
    EEPROM::handleFloat(pos + 4, PSTR("Yank [mm/s]"), 2, yank);
    EEPROM::handleFloat(pos + 8, PSTR("Max Speed [mm/s]"), 2, maxSpeed);
    EEPROM::handleFloat(pos + 12, PSTR("Acceleration [mm/s^3]"), 2, acceleration);
    EEPROM::handleFloat(pos + 16, PSTR("Advance [steps/mm]"), 2, advance);
    EEPROM::handleFloat(pos + 20, PSTR("Diameter [mm]"), 2, diameter);
}

void ToolExtruder::setAdvance(float adv) {
    advance = adv;
    if (this == Tool::getActiveTool()) {
        Motion1::advanceK = advance;
    }
}

void ToolExtruder::updateDerived() {
    Motion1::advanceK = advance;
    Tool* t = Tool::getActiveTool();
    Motion1::setMotorForAxis(stepper, E_AXIS);
    Motion1::setToolOffset(-t->getOffsetX(), -t->getOffsetY(), -t->getOffsetZ());
}

void ToolExtruder::disableMotor() {
    stepper->disable();
}

void ToolExtruder::enableMotor() {
    stepper->enable();
}
void ToolExtruder::stepMotor() {
    stepper->step();
}
void ToolExtruder::unstepMotor() {
    stepper->unstep();
}
bool ToolExtruder::stepCondMotor() {
    return stepper->stepCond();
}

// Laser

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::init() {
    setEepromStart(EEPROM::reserve(EEPROM_SIGNATURE_LASER, 1, Tool::eepromSize() + 2 * 4 + 2));
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::eepromHandle() {
    Tool::eepromHandle();
    uint pos = getEepromStart() + Tool::eepromSize();
    EEPROM::handleFloat(pos, PSTR("Power [mW]"), 2, milliWatt);
    EEPROM::handleLong(pos + 4, PSTR("Warmup [us]"), warmup);
    EEPROM::handleInt(pos + 8, PSTR("Warmup Power [PWM]"), warmupPower);
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::reset(float offx, float offy, float offz, float _milliwatt, int32_t _warmup, int16_t _warmupPower) {
    resetBase(offx, offy, offz);
    milliWatt = _milliwatt;
    warmup = _warmup;
    warmupPower = _warmupPower;
}

/// Called when the tool gets activated.
template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::activate() {
    enabledPin::on();
    GCode::executeFString(startScript);
    Motion1::setMotorForAxis(nullptr, E_AXIS);
}
/// Gets called when the tool gets disabled.
template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::deactivate() {
    enabledPin::off();
    GCode::executeFString(endScript);
    Motion1::setMotorForAxis(nullptr, E_AXIS);
}
/// Called on kill/emergency to disable the tool
template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::shutdown() {
    enabledPin::off();
    secondary->set(0);
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::updateDerived() {
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::updateGammaMap(bool report) {
    gammaMap[0] = 0;
    int offset = bias * 255.0f / milliWatt;
    if (report) {
        Com::printFLN(PSTR("Gamma Offset:"), (int)offset);
    }
    for (fast8_t i = 1; i <= 255; i++) {
        int intensity = static_cast<int>(lroundf(powf((float)i / 255.0f, gamma) * 255.0));
        gammaMap[i] = static_cast<uint8_t>(map(intensity, 0, 255, offset, 255)); // scale gamma function and offset to max
        if (report) {
            Com::printF(PSTR("gamma"), (int)i);
            Com::printFLN(PSTR("="), (int)gammaMap[i]);
        }
    }
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::extractNewGammaCorrection(GCode* com) {
    float b = bias;
    float c = gamma;
    if (com->hasB()) {
        b = com->B;
        if (b < 0) {
            b = 0.0f;
        } else if (b > milliWatt) {
            b = milliWatt;
        }
    }
    if (com->hasC()) {
        c = com->C;
        if (c <= 0) {
            c = 1.0;
        }
    }
    if (c != gamma || b != bias) {
        gamma = c;
        bias = b;
        Motion1::waitForEndOfMoves();
        updateGammaMap(com->hasR());
    }
}

template <class enabledPin, class activePin>
int ToolLaser<enabledPin, activePin>::computeIntensity(float v, bool activeSecondary, int intensity, float intensityPerMM) {
    if (!activeSecondary) {
        return 0;
    }
    if (intensity) {
        return gammaMap[intensity];
    }
    float target = v * intensityPerMM;
    if (target < 0) {
        target = 0;
    }
    if (target > 255) {
        target = 255;
    }
    return gammaMap[static_cast<int>(target)];
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::M3(GCode* com) {
    if (com->hasS()) {
        activeSecondaryValue = com->S;
        if (activeSecondaryValue < 0) {
            activeSecondaryValue = 0;
        } else if (activeSecondaryValue > 255) {
            activeSecondaryValue = 255;
        }
        activeSecondaryPerMMPS = 0;
    } else if (com->hasI()) {
        activeSecondaryValue = 0;
        activeSecondaryPerMMPS = 255.0 * com->I / milliWatt;
    } else {
        activeSecondaryValue = 255;
        activeSecondaryPerMMPS = 0;
    }
    extractNewGammaCorrection(com);
    if (com->hasR()) {
        Com::printF(PSTR("Fixed PWM:"), activeSecondaryValue);
        Com::printFLN(PSTR(" variable PWM/v:"), activeSecondaryPerMMPS);
    }
    activePin::on();
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::M4(GCode* com) {
    if (com->hasS()) {
        activeSecondaryValue = com->S;
        if (activeSecondaryValue < 0) {
            activeSecondaryValue = 0;
        } else if (activeSecondaryValue > 255) {
            activeSecondaryValue = 255;
        }
        activeSecondaryPerMMPS = 0;
    } else if (com->hasI()) {
        activeSecondaryValue = 0;
        activeSecondaryPerMMPS = 255.0 * com->I / milliWatt;
    } else {
        activeSecondaryValue = 255;
        activeSecondaryPerMMPS = 0;
    }
    extractNewGammaCorrection(com);
    if (com->hasR()) {
        Com::printF(PSTR("Fixed PWM:"), activeSecondaryValue);
        Com::printFLN(PSTR(" variable PWM/v:"), activeSecondaryPerMMPS);
    }
    activePin::on();
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::M5(GCode* com) {
    activeSecondaryValue = 0;
    activeSecondaryPerMMPS = 0;
    activePin::off();
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::secondarySwitched(bool nowSecondary) {
    if (nowSecondary && warmup > 0 && (activeSecondaryValue > 0 || activeSecondaryPerMMPS > 0)) {
        Motion1::WarmUp(warmup, warmupPower);
    }
}

template <class enabledPin, class activePin>
void ToolLaser<enabledPin, activePin>::moveFinished() {
    secondary->set(0); // Disable laser after each move for safety!
}

// ------------ JamDetectorHW ------------

template <class inputPin, class ObserverType>
JamDetectorHW<inputPin, ObserverType>::JamDetectorHW(ObserverType* _observer, Tool* _tool, int32_t _distanceSteps, int32_t _jitterSteps, int32_t _jamPercentage) {
    observer = _observer;
    tool = _tool;
    distanceSteps = _distanceSteps;
    jitterSteps = _jitterSteps;
    jamPercentage = _jamPercentage;
    errorSteps = (distanceSteps * jamPercentage) / 100;
    lastSignal = 0;
    eepromStart = EEPROM::reserve(EEPROM_SIGNATURE_JAM, 1, 3 * 4);
}

template <class inputPin, class ObserverType>
void JamDetectorHW<inputPin, ObserverType>::reset(int32_t _distanceSteps, int32_t _jitterSteps, int32_t _jamPercentage) {
    distanceSteps = _distanceSteps;
    jitterSteps = _jitterSteps;
    jamPercentage = _jamPercentage;
    errorSteps = (distanceSteps * jamPercentage) / 100;
}

template <class inputPin, class ObserverType>
void JamDetectorHW<inputPin, ObserverType>::eepromHandle() {
    EEPROM::handlePrefix(PSTR("Jam Detector"), tool->getToolId() + 1);
    EEPROM::handleLong(eepromStart + 0, PSTR("Steps between signal changes [steps]"), distanceSteps);
    EEPROM::handleLong(eepromStart + 4, PSTR("Jitter signal range [steps]"), jitterSteps);
    EEPROM::handleLong(eepromStart + 8, PSTR("Jam signal at [%]"), jamPercentage);
    EEPROM::removePrefix();
    errorSteps = (distanceSteps * jamPercentage) / 100;
}

template <class inputPin, class ObserverType>
void JamDetectorHW<inputPin, ObserverType>::testForJam() {
    if (!Printer::isDebugJamOrDisabled()) {
        int32_t diff = labs(static_cast<int32_t>(observer->position - lastSignal));
        if (diff > errorSteps && !tool->hasError(TOOL_ERROR_JAMMED_OR_NO_FILAMENT)) { // handle error
            tool->setError(TOOL_ERROR_JAMMED_OR_NO_FILAMENT);
            EVENT_JAM_DETECTED;
            Com::printFLN(PSTR("important:Extruder jam detected"));
#if SDSUPPORT
            if (sd.sdmode == 2) {
                sd.pausePrint(true);
                EVENT_JAM_DETECTED_END;
                return;
            }
#endif // SDSUPPORT
            GCodeSource::printAllFLN(PSTR("RequestPause:Extruder Jam Detected!"));
            EVENT_JAM_DETECTED_END;
        }
    }
}

template <class inputPin, class ObserverType>
void JamDetectorHW<inputPin, ObserverType>::interruptSignaled() {
    if (!Printer::isJamcontrolDisabled()) {
        int32_t diff = labs(static_cast<int32_t>(observer->position - lastSignal));
        if (diff < jitterSteps) { // ignore swinging state at edge
            return;
        }
        if (diff < errorSteps) {
            tool->resetError(TOOL_ERROR_JAMMED_OR_NO_FILAMENT);
        }
        if (Printer::isDebugJam()) { // report signal
            FirmwareEvent::queueEvent(FIRMWARE_EVENT_JAM_DEBUG, wizardVar(static_cast<int32_t>(tool->getToolId())), wizardVar(diff));
        }
    }
    lastSignal = observer->position;
}

#undef IO_TARGET
#define IO_TARGET 13
#include "../io/redefine.h"