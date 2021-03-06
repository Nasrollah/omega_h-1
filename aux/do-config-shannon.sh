#!/bin/bash
cmake .. \
-DCMAKE_CXX_COMPILER=$HOME/minicontact/doc/dashboards/shannon.sandia.gov/shannon_local/nvcc_wrapper \
-DCMAKE_INSTALL_PREFIX=$HOME/omega_h2-install \
-DKokkos_PREFIX=$HOME/Trilinos-install \
-DOmega_h_USE_Kokkos=True \
-DOmega_h_USE_CUDA=True \
-DBUILD_TESTING=ON \
-DOmega_h_ONE_FILE=ON
