//===--------------------- InstrBuilder.cpp ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements the InstrBuilder interface.
///
//===----------------------------------------------------------------------===//

#include "InstrBuilder.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/WithColor.h"

#define DEBUG_TYPE "llvm-mca"

namespace mca {

using namespace llvm;

static void initializeUsedResources(InstrDesc &ID,
                                    const MCSchedClassDesc &SCDesc,
                                    const MCSubtargetInfo &STI,
                                    ArrayRef<uint64_t> ProcResourceMasks) {
  const MCSchedModel &SM = STI.getSchedModel();

  // Populate resources consumed.
  using ResourcePlusCycles = std::pair<uint64_t, ResourceUsage>;
  std::vector<ResourcePlusCycles> Worklist;

  // Track cycles contributed by resources that are in a "Super" relationship.
  // This is required if we want to correctly match the behavior of method
  // SubtargetEmitter::ExpandProcResource() in Tablegen. When computing the set
  // of "consumed" processor resources and resource cycles, the logic in
  // ExpandProcResource() doesn't update the number of resource cycles
  // contributed by a "Super" resource to a group.
  // We need to take this into account when we find that a processor resource is
  // part of a group, and it is also used as the "Super" of other resources.
  // This map stores the number of cycles contributed by sub-resources that are
  // part of a "Super" resource. The key value is the "Super" resource mask ID.
  DenseMap<uint64_t, unsigned> SuperResources;

  for (unsigned I = 0, E = SCDesc.NumWriteProcResEntries; I < E; ++I) {
    const MCWriteProcResEntry *PRE = STI.getWriteProcResBegin(&SCDesc) + I;
    const MCProcResourceDesc &PR = *SM.getProcResource(PRE->ProcResourceIdx);
    uint64_t Mask = ProcResourceMasks[PRE->ProcResourceIdx];
    if (PR.BufferSize != -1)
      ID.Buffers.push_back(Mask);
    CycleSegment RCy(0, PRE->Cycles, false);
    Worklist.emplace_back(ResourcePlusCycles(Mask, ResourceUsage(RCy)));
    if (PR.SuperIdx) {
      uint64_t Super = ProcResourceMasks[PR.SuperIdx];
      SuperResources[Super] += PRE->Cycles;
    }
  }

  // Sort elements by mask popcount, so that we prioritize resource units over
  // resource groups, and smaller groups over larger groups.
  llvm::sort(Worklist.begin(), Worklist.end(),
             [](const ResourcePlusCycles &A, const ResourcePlusCycles &B) {
               unsigned popcntA = countPopulation(A.first);
               unsigned popcntB = countPopulation(B.first);
               if (popcntA < popcntB)
                 return true;
               if (popcntA > popcntB)
                 return false;
               return A.first < B.first;
             });

  uint64_t UsedResourceUnits = 0;

  // Remove cycles contributed by smaller resources.
  for (unsigned I = 0, E = Worklist.size(); I < E; ++I) {
    ResourcePlusCycles &A = Worklist[I];
    if (!A.second.size()) {
      A.second.NumUnits = 0;
      A.second.setReserved();
      ID.Resources.emplace_back(A);
      continue;
    }

    ID.Resources.emplace_back(A);
    uint64_t NormalizedMask = A.first;
    if (countPopulation(A.first) == 1) {
      UsedResourceUnits |= A.first;
    } else {
      // Remove the leading 1 from the resource group mask.
      NormalizedMask ^= PowerOf2Floor(NormalizedMask);
    }

    for (unsigned J = I + 1; J < E; ++J) {
      ResourcePlusCycles &B = Worklist[J];
      if ((NormalizedMask & B.first) == NormalizedMask) {
        B.second.CS.Subtract(A.second.size() - SuperResources[A.first]);
        if (countPopulation(B.first) > 1)
          B.second.NumUnits++;
      }
    }
  }

  // A SchedWrite may specify a number of cycles in which a resource group
  // is reserved. For example (on target x86; cpu Haswell):
  //
  //  SchedWriteRes<[HWPort0, HWPort1, HWPort01]> {
  //    let ResourceCycles = [2, 2, 3];
  //  }
  //
  // This means:
  // Resource units HWPort0 and HWPort1 are both used for 2cy.
  // Resource group HWPort01 is the union of HWPort0 and HWPort1.
  // Since this write touches both HWPort0 and HWPort1 for 2cy, HWPort01
  // will not be usable for 2 entire cycles from instruction issue.
  //
  // On top of those 2cy, SchedWriteRes explicitly specifies an extra latency
  // of 3 cycles for HWPort01. This tool assumes that the 3cy latency is an
  // extra delay on top of the 2 cycles latency.
  // During those extra cycles, HWPort01 is not usable by other instructions.
  for (ResourcePlusCycles &RPC : ID.Resources) {
    if (countPopulation(RPC.first) > 1 && !RPC.second.isReserved()) {
      // Remove the leading 1 from the resource group mask.
      uint64_t Mask = RPC.first ^ PowerOf2Floor(RPC.first);
      if ((Mask & UsedResourceUnits) == Mask)
        RPC.second.setReserved();
    }
  }

  LLVM_DEBUG({
    for (const std::pair<uint64_t, ResourceUsage> &R : ID.Resources)
      dbgs() << "\t\tMask=" << R.first << ", cy=" << R.second.size() << '\n';
    for (const uint64_t R : ID.Buffers)
      dbgs() << "\t\tBuffer Mask=" << R << '\n';
  });
}

static void computeMaxLatency(InstrDesc &ID, const MCInstrDesc &MCDesc,
                              const MCSchedClassDesc &SCDesc,
                              const MCSubtargetInfo &STI) {
  if (MCDesc.isCall()) {
    // We cannot estimate how long this call will take.
    // Artificially set an arbitrarily high latency (100cy).
    ID.MaxLatency = 100U;
    return;
  }

  int Latency = MCSchedModel::computeInstrLatency(STI, SCDesc);
  // If latency is unknown, then conservatively assume a MaxLatency of 100cy.
  ID.MaxLatency = Latency < 0 ? 100U : static_cast<unsigned>(Latency);
}

static void populateWrites(InstrDesc &ID, const MCInst &MCI,
                           const MCInstrDesc &MCDesc,
                           const MCSchedClassDesc &SCDesc,
                           const MCSubtargetInfo &STI) {
  // This algorithm currently works under the strong (and potentially incorrect)
  // assumption that information related to register def/uses can be obtained
  // from MCInstrDesc.
  //
  // However class MCInstrDesc is used to describe MachineInstr objects and not
  // MCInst objects. To be more specific, MCInstrDesc objects are opcode
  // descriptors that are automatically generated via tablegen based on the
  // instruction set information available from the target .td files.  That
  // means, the number of (explicit) definitions according to MCInstrDesc always
  // matches the cardinality of the `(outs)` set in tablegen.
  //
  // By constructions, definitions must appear first in the operand sequence of
  // a MachineInstr. Also, the (outs) sequence is preserved (example: the first
  // element in the outs set is the first operand in the corresponding
  // MachineInstr).  That's the reason why MCInstrDesc only needs to declare the
  // total number of register definitions, and not where those definitions are
  // in the machine operand sequence.
  //
  // Unfortunately, it is not safe to use the information from MCInstrDesc to
  // also describe MCInst objects. An MCInst object can be obtained from a
  // MachineInstr through a lowering step which may restructure the operand
  // sequence (and even remove or introduce new operands). So, there is a high
  // risk that the lowering step breaks the assumptions that register
  // definitions are always at the beginning of the machine operand sequence.
  //
  // This is a fundamental problem, and it is still an open problem. Essentially
  // we have to find a way to correlate def/use operands of a MachineInstr to
  // operands of an MCInst. Otherwise, we cannot correctly reconstruct data
  // dependencies, nor we can correctly interpret the scheduling model, which
  // heavily uses machine operand indices to define processor read-advance
  // information, and to identify processor write resources.  Essentially, we
  // either need something like a MCInstrDesc, but for MCInst, or a way
  // to map MCInst operands back to MachineInstr operands.
  //
  // Unfortunately, we don't have that information now. So, this prototype
  // currently work under the strong assumption that we can always safely trust
  // the content of an MCInstrDesc.  For example, we can query a MCInstrDesc to
  // obtain the number of explicit and implicit register defintions.  We also
  // assume that register definitions always come first in the operand sequence.
  // This last assumption usually makes sense for MachineInstr, where register
  // definitions always appear at the beginning of the operands sequence. In
  // reality, these assumptions could be broken by the lowering step, which can
  // decide to lay out operands in a different order than the original order of
  // operand as specified by the MachineInstr.
  //
  // Things get even more complicated in the presence of "optional" register
  // definitions. For MachineInstr, optional register definitions are always at
  // the end of the operand sequence. Some ARM instructions that may update the
  // status flags specify that register as a optional operand.  Since we don't
  // have operand descriptors for MCInst, we assume for now that the optional
  // definition is always the last operand of a MCInst.  Again, this assumption
  // may be okay for most targets. However, there is no guarantee that targets
  // would respect that.
  //
  // In conclusion: these are for now the strong assumptions made by the tool:
  //  * The number of explicit and implicit register definitions in a MCInst
  //    matches the number of explicit and implicit definitions according to
  //    the opcode descriptor (MCInstrDesc).
  //  * Register definitions take precedence over register uses in the operands
  //    list.
  //  * If an opcode specifies an optional definition, then the optional
  //    definition is always the last operand in the sequence, and it can be
  //    set to zero (i.e. "no register").
  //
  // These assumptions work quite well for most out-of-order in-tree targets
  // like x86. This is mainly because the vast majority of instructions is
  // expanded to MCInst using a straightforward lowering logic that preserves
  // the ordering of the operands.
  //
  // In the longer term, we need to find a proper solution for this issue.
  unsigned NumExplicitDefs = MCDesc.getNumDefs();
  unsigned NumImplicitDefs = MCDesc.getNumImplicitDefs();
  unsigned NumWriteLatencyEntries = SCDesc.NumWriteLatencyEntries;
  unsigned TotalDefs = NumExplicitDefs + NumImplicitDefs;
  if (MCDesc.hasOptionalDef())
    TotalDefs++;
  ID.Writes.resize(TotalDefs);
  // Iterate over the operands list, and skip non-register operands.
  // The first NumExplictDefs register operands are expected to be register
  // definitions.
  unsigned CurrentDef = 0;
  unsigned i = 0;
  for (; i < MCI.getNumOperands() && CurrentDef < NumExplicitDefs; ++i) {
    const MCOperand &Op = MCI.getOperand(i);
    if (!Op.isReg())
      continue;

    WriteDescriptor &Write = ID.Writes[CurrentDef];
    Write.OpIndex = i;
    if (CurrentDef < NumWriteLatencyEntries) {
      const MCWriteLatencyEntry &WLE =
          *STI.getWriteLatencyEntry(&SCDesc, CurrentDef);
      // Conservatively default to MaxLatency.
      Write.Latency = WLE.Cycles == -1 ? ID.MaxLatency : WLE.Cycles;
      Write.SClassOrWriteResourceID = WLE.WriteResourceID;
    } else {
      // Assign a default latency for this write.
      Write.Latency = ID.MaxLatency;
      Write.SClassOrWriteResourceID = 0;
    }
    Write.IsOptionalDef = false;
    LLVM_DEBUG({
      dbgs() << "\t\tOpIdx=" << Write.OpIndex << ", Latency=" << Write.Latency
             << ", WriteResourceID=" << Write.SClassOrWriteResourceID << '\n';
    });
    CurrentDef++;
  }

  if (CurrentDef != NumExplicitDefs)
    llvm::report_fatal_error(
        "error: Expected more register operand definitions. ");

  CurrentDef = 0;
  for (CurrentDef = 0; CurrentDef < NumImplicitDefs; ++CurrentDef) {
    unsigned Index = NumExplicitDefs + CurrentDef;
    WriteDescriptor &Write = ID.Writes[Index];
    Write.OpIndex = -1;
    Write.RegisterID = MCDesc.getImplicitDefs()[CurrentDef];
    if (Index < NumWriteLatencyEntries) {
      const MCWriteLatencyEntry &WLE =
          *STI.getWriteLatencyEntry(&SCDesc, Index);
      // Conservatively default to MaxLatency.
      Write.Latency = WLE.Cycles == -1 ? ID.MaxLatency : WLE.Cycles;
      Write.SClassOrWriteResourceID = WLE.WriteResourceID;
    } else {
      // Assign a default latency for this write.
      Write.Latency = ID.MaxLatency;
      Write.SClassOrWriteResourceID = 0;
    }

    Write.IsOptionalDef = false;
    assert(Write.RegisterID != 0 && "Expected a valid phys register!");
    LLVM_DEBUG(dbgs() << "\t\tOpIdx=" << Write.OpIndex << ", PhysReg="
                      << Write.RegisterID << ", Latency=" << Write.Latency
                      << ", WriteResourceID=" << Write.SClassOrWriteResourceID
                      << '\n');
  }

  if (MCDesc.hasOptionalDef()) {
    // Always assume that the optional definition is the last operand of the
    // MCInst sequence.
    const MCOperand &Op = MCI.getOperand(MCI.getNumOperands() - 1);
    if (i == MCI.getNumOperands() || !Op.isReg())
      llvm::report_fatal_error(
          "error: expected a register operand for an optional "
          "definition. Instruction has not be correctly analyzed.\n",
          false);

    WriteDescriptor &Write = ID.Writes[TotalDefs - 1];
    Write.OpIndex = MCI.getNumOperands() - 1;
    // Assign a default latency for this write.
    Write.Latency = ID.MaxLatency;
    Write.SClassOrWriteResourceID = 0;
    Write.IsOptionalDef = true;
  }
}

static void populateReads(InstrDesc &ID, const MCInst &MCI,
                          const MCInstrDesc &MCDesc,
                          const MCSchedClassDesc &SCDesc,
                          const MCSubtargetInfo &STI) {
  unsigned SchedClassID = MCDesc.getSchedClass();
  bool HasReadAdvanceEntries = SCDesc.NumReadAdvanceEntries > 0;

  unsigned i = 0;
  unsigned NumExplicitDefs = MCDesc.getNumDefs();
  // Skip explicit definitions.
  for (; i < MCI.getNumOperands() && NumExplicitDefs; ++i) {
    const MCOperand &Op = MCI.getOperand(i);
    if (Op.isReg())
      NumExplicitDefs--;
  }

  if (NumExplicitDefs)
    llvm::report_fatal_error(
        "error: Expected more register operand definitions. ", false);

  unsigned NumExplicitUses = MCI.getNumOperands() - i;
  unsigned NumImplicitUses = MCDesc.getNumImplicitUses();
  if (MCDesc.hasOptionalDef()) {
    assert(NumExplicitUses);
    NumExplicitUses--;
  }
  unsigned TotalUses = NumExplicitUses + NumImplicitUses;
  if (!TotalUses)
    return;

  ID.Reads.resize(TotalUses);
  for (unsigned CurrentUse = 0; CurrentUse < NumExplicitUses; ++CurrentUse) {
    ReadDescriptor &Read = ID.Reads[CurrentUse];
    Read.OpIndex = i + CurrentUse;
    Read.UseIndex = CurrentUse;
    Read.HasReadAdvanceEntries = HasReadAdvanceEntries;
    Read.SchedClassID = SchedClassID;
    LLVM_DEBUG(dbgs() << "\t\tOpIdx=" << Read.OpIndex);
  }

  for (unsigned CurrentUse = 0; CurrentUse < NumImplicitUses; ++CurrentUse) {
    ReadDescriptor &Read = ID.Reads[NumExplicitUses + CurrentUse];
    Read.OpIndex = -1;
    Read.UseIndex = NumExplicitUses + CurrentUse;
    Read.RegisterID = MCDesc.getImplicitUses()[CurrentUse];
    Read.HasReadAdvanceEntries = HasReadAdvanceEntries;
    Read.SchedClassID = SchedClassID;
    LLVM_DEBUG(dbgs() << "\t\tOpIdx=" << Read.OpIndex
                      << ", RegisterID=" << Read.RegisterID << '\n');
  }
}

const InstrDesc &InstrBuilder::createInstrDescImpl(const MCInst &MCI) {
  assert(STI.getSchedModel().hasInstrSchedModel() &&
         "Itineraries are not yet supported!");

  unsigned short Opcode = MCI.getOpcode();
  // Obtain the instruction descriptor from the opcode.
  const MCInstrDesc &MCDesc = MCII.get(Opcode);
  const MCSchedModel &SM = STI.getSchedModel();

  // Then obtain the scheduling class information from the instruction.
  unsigned SchedClassID = MCDesc.getSchedClass();
  unsigned CPUID = SM.getProcessorID();

  // Try to solve variant scheduling classes.
  if (SchedClassID) {
    while (SchedClassID && SM.getSchedClassDesc(SchedClassID)->isVariant())
      SchedClassID = STI.resolveVariantSchedClass(SchedClassID, &MCI, CPUID);

    if (!SchedClassID)
      llvm::report_fatal_error("unable to resolve this variant class.");
  }

  // Create a new empty descriptor.
  std::unique_ptr<InstrDesc> ID = llvm::make_unique<InstrDesc>();

  const MCSchedClassDesc &SCDesc = *SM.getSchedClassDesc(SchedClassID);
  ID->NumMicroOps = SCDesc.NumMicroOps;

  if (MCDesc.isCall()) {
    // We don't correctly model calls.
    WithColor::warning() << "found a call in the input assembly sequence.\n";
    WithColor::note() << "call instructions are not correctly modeled. "
                      << "Assume a latency of 100cy.\n";
  }

  if (MCDesc.isReturn()) {
    WithColor::warning() << "found a return instruction in the input"
                         << " assembly sequence.\n";
    WithColor::note() << "program counter updates are ignored.\n";
  }

  ID->MayLoad = MCDesc.mayLoad();
  ID->MayStore = MCDesc.mayStore();
  ID->HasSideEffects = MCDesc.hasUnmodeledSideEffects();

  initializeUsedResources(*ID, SCDesc, STI, ProcResourceMasks);
  computeMaxLatency(*ID, MCDesc, SCDesc, STI);
  populateWrites(*ID, MCI, MCDesc, SCDesc, STI);
  populateReads(*ID, MCI, MCDesc, SCDesc, STI);

  LLVM_DEBUG(dbgs() << "\t\tMaxLatency=" << ID->MaxLatency << '\n');
  LLVM_DEBUG(dbgs() << "\t\tNumMicroOps=" << ID->NumMicroOps << '\n');

  // Now add the new descriptor.
  SchedClassID = MCDesc.getSchedClass();
  if (!SM.getSchedClassDesc(SchedClassID)->isVariant()) {
    Descriptors[MCI.getOpcode()] = std::move(ID);
    return *Descriptors[MCI.getOpcode()];
  }

  VariantDescriptors[&MCI] = std::move(ID);
  return *VariantDescriptors[&MCI];
}

const InstrDesc &InstrBuilder::getOrCreateInstrDesc(const MCInst &MCI) {
  if (Descriptors.find_as(MCI.getOpcode()) != Descriptors.end())
    return *Descriptors[MCI.getOpcode()];

  if (VariantDescriptors.find(&MCI) != VariantDescriptors.end())
    return *VariantDescriptors[&MCI];

  return createInstrDescImpl(MCI);
}

std::unique_ptr<Instruction>
InstrBuilder::createInstruction(const MCInst &MCI) {
  const InstrDesc &D = getOrCreateInstrDesc(MCI);
  std::unique_ptr<Instruction> NewIS = llvm::make_unique<Instruction>(D);

  // Initialize Reads first.
  for (const ReadDescriptor &RD : D.Reads) {
    int RegID = -1;
    if (RD.OpIndex != -1) {
      // explicit read.
      const MCOperand &Op = MCI.getOperand(RD.OpIndex);
      // Skip non-register operands.
      if (!Op.isReg())
        continue;
      RegID = Op.getReg();
    } else {
      // Implicit read.
      RegID = RD.RegisterID;
    }

    // Skip invalid register operands.
    if (!RegID)
      continue;

    // Okay, this is a register operand. Create a ReadState for it.
    assert(RegID > 0 && "Invalid register ID found!");
    NewIS->getUses().emplace_back(llvm::make_unique<ReadState>(RD, RegID));
  }

  // Early exit if there are no writes.
  if (D.Writes.empty())
    return NewIS;

  // Track register writes that implicitly clear the upper portion of the
  // underlying super-registers using an APInt.
  APInt WriteMask(D.Writes.size(), 0);

  // Now query the MCInstrAnalysis object to obtain information about which
  // register writes implicitly clear the upper portion of a super-register.
  MCIA.clearsSuperRegisters(MRI, MCI, WriteMask);

  // Initialize writes.
  unsigned WriteIndex = 0;
  for (const WriteDescriptor &WD : D.Writes) {
    unsigned RegID =
        WD.OpIndex == -1 ? WD.RegisterID : MCI.getOperand(WD.OpIndex).getReg();
    // Check if this is a optional definition that references NoReg.
    if (WD.IsOptionalDef && !RegID) {
      ++WriteIndex;
      continue;
    }

    assert(RegID && "Expected a valid register ID!");
    APInt CurrWriteMask = WriteMask & (1 << WriteIndex);
    bool UpdatesSuperRegisters = CurrWriteMask.getBoolValue();
    NewIS->getDefs().emplace_back(
        llvm::make_unique<WriteState>(WD, RegID, UpdatesSuperRegisters));
    ++WriteIndex;
  }

  return NewIS;
}
} // namespace mca
