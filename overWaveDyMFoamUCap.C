/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | www.openfoam.com
     \\/     M anipulation  |
-------------------------------------------------------------------------------
    Copyright (C) 2011-2014 OpenFOAM Foundation
    Copyright (C) 2016-2017 OpenCFD Ltd.
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

Application
    overWaveDyMFoamUCap

Group
    grpMultiphaseSolvers grpMovingMeshSolvers

Description
    Solver for two incompressible, isothermal immiscible fluids using a VOF
    (volume of fluid) phase-fraction based interface capturing approach,
    with optional mesh motion and mesh topology changes including adaptive
    re-meshing.

\*---------------------------------------------------------------------------*/

#include "fvCFD.H"
#include "dynamicFvMesh.H"
#include "CMULES.H"
#include "EulerDdtScheme.H"
#include "localEulerDdtScheme.H"
#include "CrankNicolsonDdtScheme.H"
#include "subCycle.H"
#include "immiscibleIncompressibleTwoPhaseMixture.H"
#include "turbulentTransportModel.H"
#include "pimpleControl.H"
#include "fvOptions.H"
#include "CorrectPhi.H"
#include "fvcSmooth.H"
#include "cellCellStencilObject.H"
#include "localMin.H"
#include "interpolationCellPoint.H"
#include "transform.H"
#include "fvMeshSubset.H"
#include "oversetAdjustPhi.H"

#include "relaxationZone.H"
#include "externalWaveForcing.H"

// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //

int main(int argc, char *argv[])
{
    argList::addNote
    (
        "Solver for two incompressible, isothermal immiscible fluids using"
        " VOF phase-fraction based interface capturing\n"
        "With optional mesh motion and mesh topology changes including"
        " adaptive re-meshing."
    );

    #include "setRootCaseLists.H"
    #include "createTime.H"
    #include "createDynamicFvMesh.H"
    #include "initContinuityErrs.H"
    	
	#include "readGravitationalAcceleration.H"
	#include "readWaveProperties.H"
	#include "createExternalWaveForcing.H"

	pimpleControl pimple(mesh);
    #include "createTimeControls.H"
    #include "createDyMControls.H"
    #include "createFields.H"
    #include "createAlphaFluxes.H"
    #include "createFvOptions.H"
	
	#include "postProcess.H"
	
    volScalarField rAU
    (
        IOobject
        (
            "rAU",
            runTime.timeName(),
            mesh,
            IOobject::READ_IF_PRESENT,
            IOobject::AUTO_WRITE
        ),
        mesh,
        dimensionedScalar("rAUf", dimTime/rho.dimensions(), 1.0)
    );

    if (correctPhi)
    {
        #include "correctPhi.H"
    }
    #include "createUf.H"

    #include "setCellMask.H"
    #include "setInterpolatedCells.H"

    turbulence->validate();

    if (!LTS)
    {
        #include "CourantNo.H"
        #include "setInitialDeltaT.H"
    }

    // * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //
    Info<< "\nStarting time loop\n" << endl;

    while (runTime.run())
    {
        #include "readControls.H"

        if (LTS)
        {
            #include "setRDeltaT.H"
        }
        else
        {
            #include "CourantNo.H"
            #include "alphaCourantNo.H"
            #include "setDeltaT.H"
        }

        ++runTime;

        Info<< "Time = " << runTime.timeName() << nl << endl;
		
		//ADDED TO READ UCAP FREQ
		label UCapFreq
		(
			runTime.controlDict().get<label>("UCapFreq")
		);
		
		// Info<< "UCapFreq: " << UCapFreq << endl;
		Info<< "Current time index: " << runTime.timeIndex() << endl;
		
		//ADDEDEND

        // --- Pressure-velocity PIMPLE corrector loop
        while (pimple.loop())
        {
            if (pimple.firstIter() || moveMeshOuterCorrectors)
            {
                scalar timeBeforeMeshUpdate = runTime.elapsedCpuTime();

                mesh.update();

                if (mesh.changing())
                {
                    Info<< "Execution time for mesh.update() = "
                        << runTime.elapsedCpuTime() - timeBeforeMeshUpdate
                        << " s" << endl;

                    // Do not apply previous time-step mesh compression flux
                    // if the mesh topology changed
                    if (mesh.topoChanging())
                    {
                        talphaPhi1Corr0.clear();
                    }

                    // gh = (g & mesh.C()) - ghRef;
                    // ghf = (g & mesh.Cf()) - ghRef;
					gh = (g & (mesh.C() - referencePoint)); // as waveFoam
                    ghf = (g & (mesh.Cf() - referencePoint)); // as waveFoam

                    // Update cellMask field for blocking out hole cells
                    #include "setCellMask.H"
                    #include "setInterpolatedCells.H"

                    const surfaceScalarField faceMaskOld
                    (
                        localMin<scalar>(mesh).interpolate(cellMask.oldTime())
                    );

                    // Zero Uf on old faceMask (H-I)
                    Uf *= faceMaskOld;

                    const surfaceVectorField Uint(fvc::interpolate(U));
                    // Update Uf and phi on new C-I faces
                    Uf += (1-faceMaskOld)*Uint;

                    // Update Uf boundary
                    forAll(Uf.boundaryField(), patchI)
                    {
                        Uf.boundaryFieldRef()[patchI] =
                            Uint.boundaryField()[patchI];
                    }

                    phi = mesh.Sf() & Uf;

                    // Correct phi on individual regions
                    if (correctPhi)
                    {
                         #include "correctPhi.H"
                    }

                    mixture.correct();

                    // Zero phi on current H-I
                    const surfaceScalarField faceMask
                    (
                        localMin<scalar>(mesh).interpolate(cellMask)
                    );
                    phi *= faceMask;
                    U   *= cellMask;

                    // Make the flux relative to the mesh motion
                    fvc::makeRelative(phi, U);

                }

                if (mesh.changing() && checkMeshCourantNo)
                {
                    #include "meshCourantNo.H"
                }
            }


            #include "alphaControls.H"
            #include "alphaEqnSubCycle.H"

            const surfaceScalarField faceMask
            (
                localMin<scalar>(mesh).interpolate(cellMask)
            );
            rhoPhi *= faceMask;

			relaxing.correct();
            
			mixture.correct();

            #include "UEqn.H"

            // --- Pressure corrector loop
            while (pimple.correct())
            {
                #include "pEqn.H"
            }

            if (pimple.turbCorr())
            {
                turbulence->correct();
            }
        }
		
		//ADDED
		// Execute the velocity-capping code only if the current time step is a multiple of the frequency
		if (runTime.timeIndex() % UCapFreq == 0)
		{
			// Compute local maximum velocity in phase 1
			volVectorField U1 = U * alpha1; // Velocity field for phase 1
			scalar localMaxU1 = -GREAT;     // Initialize to a very small value

			forAll(U1, celli)
			{
				if (alpha1[celli] > 0.999) // Only consider cells fully occupied by phase 1
				{
					localMaxU1 = max(localMaxU1, mag(U1[celli]));
				}
			}

			// Gather all local maxima to find the global maximum
			reduce(localMaxU1, maxOp<scalar>()); // Ensures global maximum in parallel
			scalar globalMaxU1 = localMaxU1;

			// Replace velocities in phase 2
			forAll(U, celli)
			{
				if (alpha2[celli] > 0.5) // Check if the cell is predominantly phase 2
				{
					if (mag(U[celli]) > globalMaxU1)
					{
						U[celli] = U[celli] * (globalMaxU1 / mag(U[celli])); // Scale the velocity to globalMaxU1
					}
				}
			}
			Info<< "Updated U" << endl;
		}
		//ADDEDEND

        runTime.write();

        runTime.printExecutionTime(Info);
    }
	
	// Close down the external wave forcing in a nice manner
    externalWave->close();

    Info<< "End\n" << endl;

    return 0;
}


// ************************************************************************* //
