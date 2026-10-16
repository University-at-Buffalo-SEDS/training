# Datalogger software design document

This project fills the included UB SEDS SDD template for the CrashNBurn datalogger.
Upload Datalogger_SDD_Overleaf.zip as a new Overleaf project. Select main.tex and XeLaTeX.
The original blank template is preserved in the repository.

The document distinguishes generated code from proposed application behavior. It is a draft for firmware peer review, not a claim of working logger hardware.

Edit metadata.tex, sections/*.tex, and figures/*.tex. Build locally:

```sh
latexmk -xelatex -interaction=nonstopmode -halt-on-error -outdir=../../build/sdd main.tex
```

The output path resolves to the repository's ignored build/sdd directory. The root README also gives a command that can be run from the repository root.
Keep auxiliary files under ignored build/. Package main.tex, metadata.tex, ubseds-brand.sty, assets/, figures/, sections/, and this README at the ZIP root.
