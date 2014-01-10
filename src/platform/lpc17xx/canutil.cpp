#include "can/canutil.h"
#include "canutil_lpc17xx.h"
#include "signals.h"
#include "util/log.h"
#include "lpc17xx_pinsel.h"

// Same for both Blueboard and Ford VI prototype
// CAN1: select P0.21 as RD1. P0.22 as TD1
// CAN2: select P0.4 as RD2, P0.5 as RD2
#define CAN_RX_PIN_NUM(BUS) (BUS == LPC_CAN1 ? 21 : 4)
#define CAN_TX_PIN_NUM(BUS) (BUS == LPC_CAN1 ? 22 : 5)
#define CAN_PORT_NUM(BUS) 0
#define CAN_FUNCNUM(BUS) (BUS == LPC_CAN1 ? 3 : 2)

using openxc::signals::getCanBusCount;
using openxc::signals::getCanBuses;
using openxc::util::log::debug;

AF_SectionDef AF_TABLE;
SFF_Entry STANDARD_AF_TABLE[MAX_ACCEPTANCE_FILTERS];

static bool updateAcceptanceFilterTable(CanBus* bus) {
    // TODO loop over all filters, set in table
    STANDARD_AF_TABLE[0].controller = bus->address - 1;
    STANDARD_AF_TABLE[0].disable = false;
    STANDARD_AF_TABLE[0].id_11 = 0x08;

    AF_TABLE.SFF_Sec = STANDARD_AF_TABLE;
    AF_TABLE.SFF_NumEntry = 6;

    return CAN_SetupAFLUT(LPC_CANAF, &AF_TABLE) == CAN_OK;
}

bool openxc::can::setAcceptanceFilterStatus(CanBus* bus, bool enabled) {
    debug("The LPC1768's CAN acceptance filter is global - setting %s for all controllers",
            enabled ? "on": "off");
    if(enabled) {
        CAN_SetAFMode(LPC_CANAF, CAN_Normal);
    } else {
        CAN_SetAFMode(LPC_CANAF, CAN_AccBP);
    }
    return true;
}

// TODO when we merge this branch with 'iso' this function will be duplicated
// except for the return type
static AcceptanceFilterListEntry* popListEntry(AcceptanceFilterList* list) {
    AcceptanceFilterListEntry* result = list->lh_first;
    if(result != NULL) {
        LIST_REMOVE(list->lh_first, entries);
    }
    return result;
}

bool openxc::can::addAcceptanceFilter(CanBus* bus, uint32_t id) {
    // TODO for a diagnostic request, when does a filter get removed? if a
    // request is completed and no other active requsts have the same id
    setAcceptanceFilterStatus(bus, true);

    for(AcceptanceFilterListEntry* entry = bus->acceptanceFilters.lh_first;
            entry != NULL; entry = entry->entries.le_next) {
        if(entry->filter == id) {
            return true;
        }
    }

    AcceptanceFilterListEntry* availableFilter = popListEntry(
            &bus->freeAcceptanceFilters);
    if(availableFilter == NULL) {
        debug("All acceptance filter slots already taken, can't add 0x%lx",
                id);
        return false;
    }

    // TODO everything in this function except this is portable - we need an
    // addAcceptanceFilter (in canutil.cpp and addAcceptanceFilterPlatformSpecific or something
    // like that (in platform/*/canutil.cpp)

    availableFilter->filter = id;
    LIST_INSERT_HEAD(&bus->acceptanceFilters, availableFilter, entries);
    return updateAcceptanceFilterTable(bus);
}

void openxc::can::removeAcceptanceFilter(CanBus* bus, uint32_t id) {
    AcceptanceFilterListEntry* entry;
    for(entry = bus->acceptanceFilters.lh_first; entry != NULL;
            entry = entry->entries.le_next) {
        if(entry->filter == id) {
            break;
        }
    }

    if(entry != NULL) {
        LIST_REMOVE(entry, entries);
        if(bus->acceptanceFilters.lh_first == NULL) {
            // when all filters are removed, switch into bypass mode
            setAcceptanceFilterStatus(bus, false);
        }
        updateAcceptanceFilterTable(bus);
    }
}

void configureCanControllerPins(LPC_CAN_TypeDef* controller) {
    PINSEL_CFG_Type PinCfg;
    PinCfg.OpenDrain = 0;
    PinCfg.Pinmode = 0;
    PinCfg.Funcnum = CAN_FUNCNUM(controller);
    PinCfg.Pinnum = CAN_RX_PIN_NUM(controller);
    PinCfg.Portnum = CAN_PORT_NUM(controller);
    PINSEL_ConfigPin(&PinCfg);

    PinCfg.Pinnum = CAN_TX_PIN_NUM(controller);
    PINSEL_ConfigPin(&PinCfg);
}

void configureTransceiver() {
    // make P0.19 high to make sure the TJ1048T is awake
    LPC_GPIO0->FIODIR |= 1 << 19;
    LPC_GPIO0->FIOPIN |= 1 << 19;
    LPC_GPIO0->FIODIR |= 1 << 6;
    LPC_GPIO0->FIOPIN |= 1 << 6;
}

void openxc::can::deinitialize(CanBus* bus) { }

void openxc::can::initialize(CanBus* bus, bool writable) {
    can::initializeCommon(bus);
    configureCanControllerPins(CAN_CONTROLLER(bus));
    configureTransceiver();

    static bool CAN_CONTROLLER_INITIALIZED = false;
    // TODO workaround the fact that CAN_Init erase the acceptance filter
    // table, so we need to initialize both CAN controllers before setting
    // any filters, and then make sure not to call CAN_Init again.
    if(!CAN_CONTROLLER_INITIALIZED) {
        for(int i = 0; i < getCanBusCount(); i++) {
            CAN_Init(CAN_CONTROLLER((&getCanBuses()[i])), getCanBuses()[i].speed);
        }
        CAN_CONTROLLER_INITIALIZED = true;
    }

    CAN_MODE_Type mode = CAN_LISTENONLY_MODE;
    if(writable) {
        debug("Initializing bus %d in writable mode", bus->address);
        mode = CAN_OPERATING_MODE;
    } else {
        debug("Initializing bus %d in listen only mode", bus->address);
    }
    CAN_ModeConfig(CAN_CONTROLLER(bus), mode, ENABLE);

    // enable receiver interrupt
    CAN_IRQCmd(CAN_CONTROLLER(bus), CANINT_RIE, ENABLE);
    // enable transmit interrupt
    CAN_IRQCmd(CAN_CONTROLLER(bus), CANINT_TIE1, ENABLE);

    NVIC_EnableIRQ(CAN_IRQn);

    // disable all types of acceptance filters we will not be using
    AF_TABLE.FullCAN_Sec = NULL;
    AF_TABLE.FC_NumEntry = 0;
    AF_TABLE.SFF_GPR_Sec = NULL;
    AF_TABLE.SFF_GPR_NumEntry = 0;
    AF_TABLE.EFF_Sec = NULL;
    AF_TABLE.EFF_NumEntry = 0;
    AF_TABLE.EFF_GPR_Sec = NULL;
    AF_TABLE.EFF_GPR_NumEntry = 0;

    if(!configureDefaultFilters(bus, openxc::signals::getMessages(),
            openxc::signals::getMessageCount())) {
        debug("Unable to initialize CAN acceptance filters");
    }
}
