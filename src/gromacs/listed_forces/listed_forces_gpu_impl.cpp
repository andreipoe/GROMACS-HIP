/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2018,2019,2020,2021, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 *
 * \brief Implements GPU bonded lists for non-GPU builds
 *
 * \author Mark Abraham <mark.j.abraham@gmail.com>
 *
 * \ingroup module_listed_forces
 */

#include "gmxpre.h"

#include "config.h"

#include <algorithm>
#include <string>

#include "gromacs/listed_forces/listed_forces_gpu.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/topology/topology.h"
#include "gromacs/utility/message_string_collector.h"

namespace gmx
{

//! Returns whether there are any interactions in ilists suitable for a GPU.
static bool someInteractionsCanRunOnGpu(const InteractionLists& ilists)
{
    // Perturbation is not implemented in the GPU bonded
    // kernels. If all the interactions were actually
    // perturbed, then that will be detected later on each
    // domain, and work will never run on the GPU. This is
    // very unlikely to occur, and has little run-time cost,
    // so we don't complicate the code by catering for it
    // here.
    return std::any_of(fTypesOnGpu.begin(), fTypesOnGpu.end(), [ilists](int fType) {
        return !ilists[fType].iatoms.empty();
    });
}

//! Returns whether there are any bonded interactions in the global topology suitable for a GPU.
static bool bondedInteractionsCanRunOnGpu(const gmx_mtop_t& mtop)
{
    // Check the regular molecule types
    for (const auto& moltype : mtop.moltype)
    {
        if (someInteractionsCanRunOnGpu(moltype.ilist))
        {
            return true;
        }
    }
    // Check the inter-molecular interactions.
    if (mtop.intermolecular_ilist)
    {
        if (someInteractionsCanRunOnGpu(*mtop.intermolecular_ilist))
        {
            return true;
        }
    }
    return false;
}

bool buildSupportsListedForcesGpu(std::string* error)
{
    MessageStringCollector errorReasons;
    // Before changing the prefix string, make sure that it is not searched for in regression tests.
    errorReasons.startContext("Bonded interactions on GPU are not supported in:");
    errorReasons.appendIf(GMX_DOUBLE, "Double precision build of GROMACS");
    errorReasons.appendIf(GMX_GPU_OPENCL, "OpenCL build of GROMACS");
    errorReasons.appendIf(GMX_GPU_SYCL, "SYCL build of GROMACS");
    errorReasons.appendIf(!GMX_GPU, "CPU-only build of GROMACS");
    errorReasons.finishContext();
    if (error != nullptr)
    {
        *error = errorReasons.toString();
    }
    return errorReasons.isEmpty();
}

bool inputSupportsListedForcesGpu(const t_inputrec& ir, const gmx_mtop_t& mtop, std::string* error)
{
    MessageStringCollector errorReasons;
    // Before changing the prefix string, make sure that it is not searched for in regression tests.
    errorReasons.startContext("Bonded interactions can not be computed on a GPU:");

    errorReasons.appendIf(!bondedInteractionsCanRunOnGpu(mtop),
                          "None of the bonded types are implemented on the GPU.");
    errorReasons.appendIf(
            !EI_DYNAMICS(ir.eI),
            "Cannot compute bonded interactions on a GPU, because GPU implementation requires "
            "a dynamical integrator (md, sd, etc).");
    errorReasons.appendIf(EI_MIMIC(ir.eI), "MiMiC");
    errorReasons.appendIf(ir.useMts, "Cannot run with multiple time stepping");
    errorReasons.appendIf((ir.opts.ngener > 1), "Cannot run with multiple energy groups");
    errorReasons.finishContext();
    if (error != nullptr)
    {
        *error = errorReasons.toString();
    }
    return errorReasons.isEmpty();
}

#if !GMX_GPU_CUDA && !GMX_GPU_HIP

class ListedForcesGpu::Impl
{
};

ListedForcesGpu::ListedForcesGpu(const gmx_ffparams_t& /* ffparams */,
                                 const float /* electrostaticsScaleFactor */,
                                 const DeviceContext& /* deviceContext */,
                                 const DeviceStream& /* deviceStream */,
                                 gmx_wallcycle* /* wcycle */) :
    impl_(nullptr)
{
}

ListedForcesGpu::~ListedForcesGpu() = default;

void ListedForcesGpu::updateInteractionListsAndDeviceBuffers(ArrayRef<const int> /* nbnxnAtomOrder */,
                                                             const InteractionDefinitions& /* idef */,
                                                             void* /* xqDevice */,
                                                             DeviceBuffer<RVec> /* forceDevice */,
                                                             DeviceBuffer<RVec> /* fshiftDevice */)
{
}

void ListedForcesGpu::setPbc(PbcType /* pbcType */, const matrix /* box */, bool /* canMoleculeSpanPbc */)
{
}

bool ListedForcesGpu::haveInteractions() const
{
    return !impl_;
}

void ListedForcesGpu::launchKernel(const gmx::StepWorkload& /* stepWork */) {}

void ListedForcesGpu::setPbcAndlaunchKernel(PbcType /* pbcType */,
                                            const matrix /* box */,
                                            bool /* canMoleculeSpanPbc */,
                                            const gmx::StepWorkload& /* stepWork */)
{
}

void ListedForcesGpu::launchEnergyTransfer() {}

void ListedForcesGpu::waitAccumulateEnergyTerms(gmx_enerdata_t* /* enerd */) {}

void ListedForcesGpu::clearEnergies() {}

#endif // !GMX_GPU_CUDA

} // namespace gmx
