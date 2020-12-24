# `ergm`: Fit, Simulate and Diagnose Exponential-Family Models for Networks

[![Build Status](https://travis-ci.org/statnet/ergm.svg?branch=master)](https://travis-ci.org/statnet/ergm)
[![Build Status](https://ci.appveyor.com/api/projects/status/8lxl3cm48ktlo9j3?svg=true)](https://ci.appveyor.com/project/statnet/ergm)
[![rstudio mirror downloads](https://cranlogs.r-pkg.org/badges/ergm?color=2ED968)](https://cranlogs.r-pkg.org/)
[![cran version](https://www.r-pkg.org/badges/version/ergm)](https://cran.r-project.org/package=ergm)
[![R build status](https://github.com/statnet/ergm/workflows/R-CMD-check/badge.svg)](https://github.com/statnet/ergm/actions)

An integrated set of tools to analyze and simulate networks based on exponential-family random graph models (ERGMs). 'ergm' is a part of the Statnet suite of packages for network analysis.

## Public and Private repositories

To facilitate open development of the package while giving the core developers an opportunity to publish on their developments before opening them up for general use, this project comprises two repositories:
* A public repository `statnet/ergm`
* A private repository `statnet/ergm-private`

The intention is that all developments in `statnet/ergm-private` will eventually make their way into `statnet/ergm` and onto CRAN.

Developers and Contributing Users to the Statnet Project should read https://statnet.github.io/private/ for information about the relationship between the public and the private repository and the workflows involved.

## Latest Windows and MacOS binaries

A set of binaries is built after every commit to the public repository. We strongly encourage testing against them before filing a bug report, as they may contain fixes that have not yet been sent to CRAN. They can be downloaded through the following links:

* [MacOS binary (a `.tgz` file in a `.zip` file)](https://nightly.link/statnet/ergm/workflows/R-CMD-check.yaml/master/macOS-rrelease-binaries.zip)
* [Windows binary (a `.zip` file in a `.zip` file)](https://nightly.link/statnet/ergm/workflows/R-CMD-check.yaml/master/Windows-rrelease-binaries.zip)

You will need to extract the MacOS `.tgz` or the Windows `.zip` file from the outer `.zip` file before installing. These binaries are usually built under the latest version of R and their operating system and may not work under other versions.

You may also want to install the corresponding latest binaries for packages on which `ergm` depends, including [`rle`](https://github.com/statnet/rle), [`statnet.common`](https://github.com/statnet/statnet.common), and [`network`](https://github.com/statnet/network).
