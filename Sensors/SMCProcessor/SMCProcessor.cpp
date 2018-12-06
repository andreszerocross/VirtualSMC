//
//  sens_cpu.cpp
//  SensorsCPU
//
//  Based on code by mercurysquad, superhai © 2008
//  Based on code from Open Hardware Monitor project by Michael Möller © 2011
//  Based on code by slice © 2013
//  Portions copyright © 2010 Natan Zalkin <natan.zalkin@me.com>.
//  Copyright © 2018 vit9696. All rights reserved.
//

#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <Headers/kern_time.hpp>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOTimerEventSource.h>

#include "SMCProcessor.hpp"
#include "KeyImplementations.hpp"

OSDefineMetaClassAndStructors(SMCProcessor, IOService)

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

void SMCProcessor::readTjmax() {
	uint32_t cpu = cpu_number();
	if (cpu < CPUInfo::MaxCpus && cpuTopology.numberToLogical[cpu] == 0) {
		uint64_t tjmax;
		if (readMsr(MSR_TEMPERATURE_TARGET, tjmax)) {
			counters.tjmax[cpuTopology.numberToPackage[cpu]] =
				static_cast<uint8_t>(getBitField<uint64_t>(tjmax, 23, 16));
		} else {
			// All Nehalem+ processors support MSR_TEMPERATURE_TARGET, but let's have a failsafe value.
			counters.tjmax[cpuTopology.numberToPackage[cpu]] = 100;
		}
	}
}

void SMCProcessor::readRapl() {
	uint32_t cpu = cpu_number();
	if (cpu < CPUInfo::MaxCpus && cpuTopology.numberToLogical[cpu] == 0) {
		auto pkg = cpuTopology.numberToPackage[cpu];
		uint64_t msr;
		if (readMsr(MSR_RAPL_POWER_UNIT, msr)) {
			// auto powerUnits = static_cast<uint8_t>(getBitField<uint64_t>(msr, 3, 0));
			auto energyUnits = static_cast<uint8_t>(getBitField<uint64_t>(msr, 12, 8));
			// auto timeUnits = static_cast<uint8_t>(getBitField<uint64_t>(msr, 19, 16));

			if (energyUnits > 0) {
				// e.g. 0xA0E03 -> 0.00025
				counters.energyUnits[pkg] = 1.0 / getBit<uint32_t>(energyUnits);
			}
		}

	}
}

void SMCProcessor::updateCounters() {
	uint32_t cpu = cpu_number();

	// This should not happen
	if (cpu >= CPUInfo::MaxCpus)
		return;

	// Ignore hyper-threaded cores
	auto package = cpuTopology.numberToPackage[cpu];
	auto logical = cpuTopology.numberToLogical[cpu];
	if (logical >= cpuTopology.physicalCount[package])
		return;

	uint64_t msr = 0;

	// Temperature per core
	if (counters.eventFlags & Counters::ThermalCore) {
		auto physical = cpuTopology.numberToPhysicalUnique(cpu);
		if ((msr = rdmsr64(MSR_IA32_THERM_STATUS)) & 0x80000000) {
			counters.thermalStatus[physical] =
				getBitField<uint32_t>(static_cast<uint32_t>(msr), 22, 16);
		}
	}

	if (logical == 0) {
		// Temperature per package
		if ((counters.eventFlags & Counters::ThermalPackage) &&
			((msr = rdmsr64(MSR_IA32_PACKAGE_THERM_STATUS)) & 0x80000000)) {
			counters.thermalStatusPackage[package] =
				getBitField<uint32_t>(static_cast<uint32_t>(msr), 22, 16);
		}

		// Energy counters
		for (size_t i = 0; i < Counters::EnergyTotal; i++) {
			if (counters.eventFlags & Counters::energyFlags(i)) {
				msr = rdmsr64(Counters::energyMsrs(i));
				counters.energyAfter[package][i] = msr;
				if (counters.energyBefore[package][i] == 0)
					counters.energyBefore[package][i] = msr;
			}
		}

		// Voltage support
		if (counters.eventFlags & Counters::Voltage) {
			counters.voltage[package] = getBitField<uint64_t>(rdmsr64(MSR_PERF_STATUS), 47, 32) /
				static_cast<float>(getBit(13));
		}
	}
}

void SMCProcessor::timerCallback() {
	IOSimpleLockLock(counterLock);
	if (counters.eventFlags) {
		auto time = getCurrentTimeNs();
		auto timerDelta = time - timerEventLastTime;
		auto energyDelta = time - timerEnergyLastTime;

		timerEventLastTime = time;

		mp_rendezvous_no_intrs([](void *cpu) {
			static_cast<SMCProcessor *>(cpu)->updateCounters();
		}, this);

		// Recalculate real energy values after time
		if (energyDelta >= MinDeltaForRescheduleNs && (counters.eventFlags & Counters::PowerAny)) {
			timerEnergyLastTime = time;
			for (size_t i = 0; i < cpuTopology.packageCount; i++) {
				for (size_t j = 0; j < Counters::EnergyTotal; j++) {
					double p;
					if (counters.energyAfter[i][j] < counters.energyBefore[i][j])
						p = UINT64_MAX - counters.energyBefore[i][j] + counters.energyAfter[i][j];
					else
						p = counters.energyAfter[i][j] - counters.energyBefore[i][j];

					counters.energyBefore[i][j] = counters.energyAfter[i][j];
					counters.power[i][j] = p / (energyDelta / 1000000000.0) * counters.energyUnits[i];
				}
			}
		}

		// timerEventSource->setTimeoutMS calls thread_call_enter_delayed_with_leeway, which spins.
		// If the previous one was too long ago, schedule another one for differential recalculation!
		if (timerDelta > MaxDeltaForRescheduleNs)
			timerEventScheduled = timerEventSource->setTimeoutMS(TimerTimeoutMs) == kIOReturnSuccess;
		else
			timerEventScheduled = false;
	}

	IOSimpleLockUnlock(counterLock);
}




void SMCProcessor::setupKeys(int coreOffset) {
	uint32_t val = 0;
	
	// MSR_IA32_THERM_STATUS Digital Readout (RO) supported If CPUID.06H:EAX[0] = 1
	if (CPUInfo::getCpuid(6, 0, &val) && (val & getBit<uint32_t>(0)))
		counters.eventFlags |= Counters::ThermalCore;

	// MSR_IA32_PACKAGE_THERM_STATUS supported If CPUID.06H: EAX[6] = 1
	// Bit 06: PTM. Package thermal management is supported if set.
	if (CPUInfo::getCpuid(6, 0, &val) && (val & getBit<uint32_t>(6)))
		counters.eventFlags |= Counters::ThermalPackage;

	// Great Intel has no way to determine whether RAPL is available, so all the projects
	// hardcode it based on cpu identification. Assume it will not be removed in the future.
	if (cpuGeneration >= CPUInfo::CpuGeneration::SandyBridge) {
		mp_rendezvous_no_intrs([](void *cpu) {
			static_cast<SMCProcessor *>(cpu)->readRapl();
		}, this);

		if (counters.energyUnits[0] > 0) {
			// Linux kernel checks the availability of RAPL msrs by reading them and comparing to zero.
			// Assume they are available on any core and cpu package if at all.
			uint64_t msr;
			if (readMsr(MSR_PKG_ENERGY_STATUS, msr))
				counters.eventFlags |= Counters::PowerTotal;
			if (readMsr(MSR_PP0_ENERGY_STATUS, msr))
				counters.eventFlags |= Counters::PowerCores;
			if (readMsr(MSR_PP1_ENERGY_STATUS, msr))
				counters.eventFlags |= Counters::PowerUncore;
			if (readMsr(MSR_DRAM_ENERGY_STATUS, msr))
				counters.eventFlags |= Counters::PowerDram;
		}

		// Also called MSR_IA32_PERF_STS, but the format we rely on refers to MSR_PERF_STATUS.
		uint64_t msr;
		if (readMsr(MSR_PERF_STATUS, msr))
			counters.eventFlags |= Counters::Voltage;
	}

	mp_rendezvous_no_intrs([](void *cpu) {
		static_cast<SMCProcessor *>(cpu)->updateCounters();
	}, this);

	DBGLOG("scpu", "resulting event flags: %u, total cores: %u, total pkg: %u", counters.eventFlags, cpuTopology.totalPhysical(), cpuTopology.packageCount);

	// The following key additions are to be sorted!
	uint8_t core = 0, pkg = 0, coreInPkg = 0;
	uint8_t maxCores = min(cpuTopology.totalPhysical(), MaxIndexCount);

	if (counters.eventFlags & Counters::PowerCores) {
		VirtualSMCAPI::addKey(KeyPC0C, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyCoresIdx)));
		VirtualSMCAPI::addKey(KeyPC0R, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyCoresIdx)));
		VirtualSMCAPI::addKey(KeyPCAM, vsmcPlugin.data, VirtualSMCAPI::valueWithFlt(0, new CpEnergyKey(this, Counters::EnergyCoresIdx)));
		VirtualSMCAPI::addKey(KeyPCPC, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyCoresIdx)));
	}

	if (counters.eventFlags & Counters::PowerUncore) {
		VirtualSMCAPI::addKey(KeyPC0G, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyUncoreIdx)));
		VirtualSMCAPI::addKey(KeyPCGC, vsmcPlugin.data, VirtualSMCAPI::valueWithFlt(0, new CpEnergyKey(this, Counters::EnergyUncoreIdx)));
		VirtualSMCAPI::addKey(KeyPCGM, vsmcPlugin.data, VirtualSMCAPI::valueWithFlt(0, new CpEnergyKey(this, Counters::EnergyUncoreIdx)));
		VirtualSMCAPI::addKey(KeyPCPG, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyUncoreIdx)));
	}

	if (counters.eventFlags & Counters::PowerDram) {
		VirtualSMCAPI::addKey(KeyPC3C, vsmcPlugin.data, VirtualSMCAPI::valueWithFlt(0, new CpEnergyKey(this, Counters::EnergyDramIdx)));
		VirtualSMCAPI::addKey(KeyPCEC, vsmcPlugin.data, VirtualSMCAPI::valueWithFlt(0, new CpEnergyKey(this, Counters::EnergyDramIdx)));
	}

	if (counters.eventFlags & Counters::PowerTotal) {
		VirtualSMCAPI::addKey(KeyPCPR, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyTotalIdx)));
		VirtualSMCAPI::addKey(KeyPCPT, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyTotalIdx)));
		VirtualSMCAPI::addKey(KeyPCTR, vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp96, new CpEnergyKey(this, Counters::EnergyTotalIdx)));
	}


	//TODO: we report exact same temperature to all keys (raw and filtered) and do zero error correction.
	// We also are unaware of fractional part of the temperature reported like in Intel Power Gadget.
	while (core < maxCores) {
		// Unlike real Macs our keys are not writable!
		if (counters.eventFlags & Counters::ThermalCore) {
			VirtualSMCAPI::addKey(KeyTC0C(coreOffset + core), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempCore(this, pkg, core)));
			VirtualSMCAPI::addKey(KeyTC0c(coreOffset + core), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempCore(this, pkg, core)));
		}

		core++;
		coreInPkg++;
		if (coreInPkg >= cpuTopology.physicalCount[pkg]) {
			coreInPkg = 0;
			pkg++;
		}
	}
	
	for (pkg = 0; pkg < cpuTopology.packageCount; pkg++) {
		if (counters.eventFlags & Counters::ThermalPackage) {
			VirtualSMCAPI::addKey(KeyTC0D(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
			VirtualSMCAPI::addKey(KeyTC0E(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
			VirtualSMCAPI::addKey(KeyTC0F(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
			VirtualSMCAPI::addKey(KeyTC0G(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78));
			VirtualSMCAPI::addKey(KeyTC0H(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
			VirtualSMCAPI::addKey(KeyTC0J(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78));
			VirtualSMCAPI::addKey(KeyTC0P(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
			VirtualSMCAPI::addKey(KeyTC0p(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new TempPackage(this, pkg)));
		}

		if (counters.eventFlags & Counters::Voltage) {
			VirtualSMCAPI::addKey(KeyVC0C(pkg), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp3c, new VoltagePackage(this, pkg)));
		}
	}
	qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
}

IOService *SMCProcessor::probe(IOService *provider, SInt32 *score) {
	return IOService::probe(provider, score);
}


static const char *one_indexed_models[] = {
	"MacBook8,1",
	"MacBook9,1",
	"MacBook10,1",
	"MacBookAir6,1",
	"MacBookAir6,2",
	"MacBookAir7,1",
	"MacBookAir7,2",
	"MacBookAir8,1",
	"MacBookPro9,1",
	"MacBookPro9,2",
	"MacBookPro10,1",
	"MacBookPro11,2",
	"MacBookPro11,3",
	"MacBookPro11,4",
	"MacBookPro11,5",
	"MacBookPro13,1",
	"MacBookPro13,2",
	"MacBookPro13,3",
	"MacBookPro14,1",
	"MacBookPro14,2",
	"MacBookPro14,3",
	"MacBookPro15,1",
	"MacBookPro15,2"
};


bool SMCProcessor::start(IOService *provider) {
	DBGLOG("scpu", "starting up cpu sensors");

	if (!IOService::start(provider)) {
		SYSLOG("scpu", "failed to start the parent");
		return false;
	}

	cpuGeneration = CPUInfo::getGeneration(&cpuFamily, &cpuModel, &cpuStepping);
	if (cpuGeneration == CPUInfo::CpuGeneration::Unknown ||
		cpuGeneration < CPUInfo::CpuGeneration::Penryn) {
		SYSLOG("scpu", "failed to find a compatible processor");
		return false;
	}

	DBGLOG("scpu", "obtained generation %d %X:%X:%X", cpuGeneration, cpuFamily, cpuModel, cpuStepping);

	// Prepare time sources and event loops
	bool success = true;
	counterLock = IOSimpleLockAlloc();
	workloop = IOWorkLoop::workLoop();
	timerEventSource = IOTimerEventSource::timerEventSource(this, [](OSObject *object, IOTimerEventSource *sender) {
		auto cp = OSDynamicCast(SMCProcessor, object);
		if (cp) cp->timerCallback();
	});
	if (!timerEventSource || !workloop || !counterLock) {
		SYSLOG("scpu", "failed to create workloop, timer event source, or counter lock");
		success = false;
	}

	if (success && workloop->addEventSource(timerEventSource) != kIOReturnSuccess) {
		SYSLOG("scpu", "failed to add timer event source");
		success = false;
	}

	if (success && !CPUInfo::getCpuTopology(cpuTopology)) {
		SYSLOG("scpu", "failed to get cpu topology");
		success = false;
	}

	if (success) {
		mp_rendezvous_no_intrs([](void *cpu) {
			static_cast<SMCProcessor *>(cpu)->readTjmax();
		}, this);

		if (counters.tjmax[0] == 0) {
			SYSLOG("scpu", "tjmax temperature is 0, fallback to predefined");
			// Cannot find this bit in Intel Software Developer manual
			if (cpuGeneration == CPUInfo::CpuGeneration::Penryn && (rdmsr64(MSR_IA32_PLATFORM_ID) & 0x10000000))
				counters.tjmax[0] = 105;
			else
				counters.tjmax[0] = 100;

			for (uint8_t i = 1; i < cpuTopology.packageCount; i++)
				counters.tjmax[i] = counters.tjmax[0];
		}
	}

	DBGLOG("scpu", "read tjmax is %d", counters.tjmax[0]);

	if (!success) {
		if (counterLock) {
			IOSimpleLockFree(counterLock);
			counterLock = nullptr;
		}
		OSSafeReleaseNULL(workloop);
		OSSafeReleaseNULL(timerEventSource);
		return false;
	}

	// Some old macs like MacBookPro10,1 start core sensors not with 0, but actually with 1.
	unsigned int coreOffset = 0;
	
	char model[80];
	if (WIOKit::getComputerInfo(model, sizeof(model), nullptr, 0)) {
		for (int i = 0; i < sizeof(one_indexed_models) / sizeof(const char*); i++)
			if (!strncmp(model, one_indexed_models[i], 80)) {
				DBGLOG("scpu", "using one-based core numbers");
				coreOffset = 1;
			}
	}
	else
		SYSLOG("scpu", "failed to get system model");
	
	setupKeys(coreOffset);

	timerEventSource->setTimeoutMS(TimerTimeoutMs * 2);

	vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);

	DBGLOG("scpu", "starting up cpu sensors done %d", vsmcNotifier != nullptr);

	return vsmcNotifier != nullptr;
}

void SMCProcessor::quickReschedule() {
	if (!timerEventScheduled) {
		// Make it 10 times faster
		timerEventScheduled = timerEventSource->setTimeoutMS(TimerTimeoutMs/10) == kIOReturnSuccess;
	}
}

bool SMCProcessor::vsmcNotificationHandler(void *sensors, void *refCon, IOService *vsmc, IONotifier *notifier) {
	if (sensors && vsmc) {
		DBGLOG("scpu", "got vsmc notification");
		auto &plugin = static_cast<SMCProcessor *>(sensors)->vsmcPlugin;
		auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, sensors, &plugin, nullptr, nullptr);
		if (ret == kIOReturnSuccess) {
			DBGLOG("scpu", "submitted plugin");
			return true;
		} else if (ret != kIOReturnUnsupported) {
			SYSLOG("scpu", "plugin submission failure %X", ret);
		} else {
			DBGLOG("scpu", "plugin submission to non vsmc");
		}
	} else {
		SYSLOG("scpu", "got null vsmc notification");
	}
	return false;
}

void SMCProcessor::stop(IOService *provider) {
	SYSLOG("scpu", "called stop!!!");
}

EXPORT extern "C" kern_return_t ADDPR(kern_start)(kmod_info_t *, void *) {
	// Report success but actually do not start and let I/O Kit unload us.
	// This works better and increases boot speed in some cases.
	PE_parse_boot_argn("liludelay", &ADDPR(debugPrintDelay), sizeof(ADDPR(debugPrintDelay)));
	ADDPR(debugEnabled) = checkKernelArgument("-vsmcdbg") || checkKernelArgument("-scpudbg");
	return KERN_SUCCESS;
}

EXPORT extern "C" kern_return_t ADDPR(kern_stop)(kmod_info_t *, void *) {
	// It is not safe to unload VirtualSMC plugins!
	return KERN_FAILURE;
}
