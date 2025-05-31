# overWaveDyMFoamUCap
Based on "overWaveDyMFoam", the "overWaveDyMFoamUCap" caps unrealistic air velocity at user-defined frequency to improve simulation stability and reduce runtime.

Extra entry required in controlDict:

// Air velocity capping frequency, for example, every 5 time steps

UCapFreq		5; 
