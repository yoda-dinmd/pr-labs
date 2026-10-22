\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{utmreport}[2026/10/02 ELSE report profile and UTM thesis class]

\newif\ifUTMThesis
\DeclareOption{thesis}{\UTMThesistrue}
\ProcessOptions\relax

\LoadClass[12pt,a4paper]{report}

\ifUTMThesis
  \RequirePackage[a4paper,top=2cm,bottom=2cm,left=2.5cm,right=2cm,headsep=1cm,footskip=1.25cm]{geometry}
\else
  % Measurements from the supplied ELSE mandatory report, not the conference template.
  \RequirePackage[a4paper,top=2cm,bottom=2cm,left=2cm,right=1cm,headheight=0pt,headsep=0pt,footskip=7.3mm]{geometry}
\fi
\RequirePackage{iftex}
\ifPDFTeX
  \RequirePackage[T1]{fontenc}
  \RequirePackage[utf8]{inputenc}
  \RequirePackage{mathptmx}
\else
  \RequirePackage{fontspec}
  \IfFontExistsTF{Times New Roman}{\setmainfont{Times New Roman}}{\setmainfont{Liberation Serif}}
\fi
\RequirePackage[romanian,english]{babel}
\RequirePackage{graphicx}
\RequirePackage{amsmath}
\RequirePackage{array}
\RequirePackage{tabularx}
\RequirePackage{longtable}
\RequirePackage{caption}
\RequirePackage{enumitem}
\RequirePackage{indentfirst}
\RequirePackage{setspace}
\RequirePackage{titlesec}
\RequirePackage{tocloft}
\RequirePackage{ragged2e}
\RequirePackage{listings}
\RequirePackage{xcolor}
\RequirePackage{hyperref}
\hypersetup{hidelinks,hypertexnames=false}

\selectlanguage{romanian}
\setstretch{1.5}
\setlength{\parindent}{1.25cm}
\setlength{\parskip}{0pt}
\pagestyle{plain}

\ifUTMThesis
\titleformat{\chapter}[block]
  {\centering\bfseries\fontsize{14pt}{16pt}\selectfont}
  {\thechapter}
  {1em}
  {}
\titlespacing*{\chapter}{0pt}{0pt}{12pt}

\titleformat{\section}
  {\bfseries\fontsize{12pt}{14pt}\selectfont}
  {\thesection}
  {1em}
  {}
\titlespacing*{\section}{0pt}{12pt}{0pt}

\titleformat{\subsection}
  {\bfseries\fontsize{12pt}{14pt}\selectfont}
  {\thesubsection}
  {1em}
  {}
\titlespacing*{\subsection}{0pt}{12pt}{0pt}
\else
  \titleformat{\chapter}[block]
    {\centering\bfseries\fontsize{13pt}{16pt}\selectfont}
    {\thechapter}{1em}{\MakeUppercase}
  \titlespacing*{\chapter}{0pt}{0pt}{12pt}
  \titleformat{\section}{\bfseries\fontsize{12pt}{14pt}\selectfont}{\thesection}{1em}{}
  \titlespacing*{\section}{1.25cm}{12pt}{6pt}
  \titleformat{\subsection}{\bfseries\fontsize{12pt}{14pt}\selectfont}{\thesubsection}{1em}{}
  \titlespacing*{\subsection}{1.25cm}{12pt}{6pt}
\fi

\setcounter{secnumdepth}{2}
\setcounter{tocdepth}{2}
\renewcommand{\cftchapleader}{\cftdotfill{\cftdotsep}}
\renewcommand{\cftsecleader}{\cftdotfill{\cftdotsep}}
\ifUTMThesis
  \renewcommand{\cfttoctitlefont}{\hfill\bfseries\fontsize{14pt}{16pt}\selectfont}
\else
  \renewcommand{\cfttoctitlefont}{\hfill\bfseries\fontsize{13pt}{16pt}\selectfont\MakeUppercase}
\fi
\renewcommand{\cftaftertoctitle}{\hfill}
\setlength{\cftbeforechapskip}{0pt}

\setlist[itemize]{leftmargin=1.25cm,itemsep=0pt,topsep=0pt,parsep=0pt,partopsep=0pt}
\setlist[enumerate]{leftmargin=1.25cm,itemsep=0pt,topsep=0pt,parsep=0pt,partopsep=0pt}
\newenvironment{utmenumerate}{\begin{enumerate}[label=\alph*)]}{\end{enumerate}}

\numberwithin{equation}{chapter}
\numberwithin{figure}{chapter}
\numberwithin{table}{chapter}
\renewcommand{\figurename}{Figura}
\renewcommand{\tablename}{Tabelul}
\DeclareCaptionLabelSeparator{dash}{ - }
\captionsetup[figure]{labelsep=dash,justification=centering,singlelinecheck=true,font=normalfont}
\captionsetup[table]{labelsep=dash,justification=raggedleft,singlelinecheck=false,labelfont=bf}
\ifUTMThesis\else
  \DeclareCaptionLabelSeparator{elsedash}{ \textendash\ }
  \captionsetup[figure]{labelsep=elsedash,justification=centering,singlelinecheck=false,font={bf,normalsize}}
  \captionsetup[table]{position=top,labelsep=elsedash,justification=raggedleft,singlelinecheck=false,font={bf,normalsize}}
\fi

\newcolumntype{Y}{>{\RaggedRight\arraybackslash}X}
\renewcommand{\arraystretch}{1.2}

\lstdefinestyle{utmcode}{
  basicstyle=\ttfamily\fontsize{10pt}{12pt}\selectfont,
  breaklines=true,
  frame=single,
  numbers=left,
  numberstyle=\tiny,
  tabsize=2,
  backgroundcolor=\color{gray!8}
}
\lstset{style=utmcode}

\ifUTMThesis\else
  \ifPDFTeX
    \RequirePackage{courier}
    \newcommand{\UTMCodeFont}{\fontfamily{pcr}\selectfont}
  \else
    \IfFontExistsTF{Courier New}{\newfontfamily\UTMCodeFont{Courier New}}{\newcommand{\UTMCodeFont}{\ttfamily}}
  \fi
  \lstdefinestyle{elsereportcode}{
    basicstyle=\UTMCodeFont\fontsize{10pt}{12pt}\linespread{1}\selectfont,
    breaklines=true,frame=none,numbers=none,backgroundcolor=\color{white},
    keepspaces=true,columns=fullflexible,showstringspaces=false
  }
  \lstset{style=elsereportcode}
\fi

% Preliminary pages count, but their page numbers are not printed.
\newif\ifUTMFrontMatter
\ifUTMThesis\else\UTMFrontMattertrue\fi
\makeatletter
\ifUTMThesis\else
  \def\ps@plain{\let\@mkboth\@gobbletwo
    \let\@oddhead\@empty\let\@evenhead\@empty
    \def\@oddfoot{\ifUTMFrontMatter\else\hfil\thepage\hfil\fi}
    \let\@evenfoot\@oddfoot}
\fi
\makeatother
\pagestyle{plain}
\newcommand{\utmfrontmatter}{\UTMFrontMattertrue}
\newcommand{\utmmainmatter}{\clearpage\UTMFrontMatterfalse\pagestyle{plain}}
\newcommand{\utmintroduction}{\utmmainmatter\chapter*{\iflanguage{english}{Introduction}{Introducere}}\addcontentsline{toc}{chapter}{\iflanguage{english}{Introduction}{Introducere}}}
\newcommand{\utmconclusions}{\chapter*{\iflanguage{english}{Conclusions}{Concluzii}}\addcontentsline{toc}{chapter}{\iflanguage{english}{Conclusions}{Concluzii}}}

% The first appendix page includes the collection heading and its own letter/title.
\ifUTMThesis\else
  \newif\ifUTMFirstAppendix
  \let\UTMOriginalAppendix\appendix
  \renewcommand{\appendix}{%
    \clearpage\phantomsection
    \addcontentsline{toc}{chapter}{\iflanguage{english}{Appendices}{Anexe}}
    \UTMOriginalAppendix\UTMFirstAppendixtrue
    \titleformat{\chapter}[display]
      {\centering\bfseries\fontsize{13pt}{16pt}\selectfont}
      {\ifUTMFirstAppendix
        \global\UTMFirstAppendixfalse
        \iflanguage{english}{APPENDICES}{ANEXE}\\[12pt]
       \fi
       \iflanguage{english}{APPENDIX}{ANEXA}~\thechapter}
      {6pt}{\MakeUppercase}}
\fi

\newcommand{\UTMLogoPath}{../shared/assets/UTM_Logo.png}
\newcommand{\utmlogo}[1]{\renewcommand{\UTMLogoPath}{#1}}

\newcommand{\institution}[1]{\gdef\UTMInstitution{#1}}
\newcommand{\faculty}[1]{\gdef\UTMFaculty{#1}}
\newcommand{\department}[1]{\gdef\UTMDepartment{#1}}
\newcommand{\reporttitle}[1]{\gdef\UTMReportTitle{#1}}
\newcommand{\reportsubtitle}[1]{\gdef\UTMReportSubtitle{#1}}
\newcommand{\student}[2]{\gdef\UTMStudentName{#1}\gdef\UTMStudentGroup{#2}}
\newcommand{\supervisor}[2]{\gdef\UTMSupervisorName{#1}\gdef\UTMSupervisorTitle{#2}}
\newcommand{\consultant}[2]{\gdef\UTMConsultantName{#1}\gdef\UTMConsultantTitle{#2}}
\newcommand{\cityyear}[2]{\gdef\UTMCity{#1}\gdef\UTMYear{#2}}
\newcommand{\documenttype}[1]{\gdef\UTMDocumentType{#1}}
\newcommand{\studyprogramme}[1]{\gdef\UTMStudyProgramme{#1}}
\newcommand{\team}[1]{\gdef\UTMTeam{#1}}
\newcommand{\submissiondate}[1]{\gdef\UTMSubmissionDate{#1}}
\newcommand{\internshipmentor}[1]{\gdef\UTMInternshipMentor{#1}}
\makeatletter
\newcommand{\teammember}[3]{\g@addto@macro\UTMTeamRows{\textbf{#1:} & #2, #3 & \rule{1.2cm}{0.3pt}\\\hline}}
\makeatother
\newcommand{\UTMTeamRows}{}
\newcommand{\UTMStudentLabel}{\iflanguage{english}{Student}{Student}}
\newcommand{\UTMGroupLabel}{\iflanguage{english}{Group}{Grupa}}
\newcommand{\UTMCoordinatorLabel}{\iflanguage{english}{Coordinator}{Coordonator}}
\newcommand{\UTMSupervisorDisplay}{\ifx\UTMSupervisorTitle\empty\else\UTMSupervisorTitle, \fi\UTMSupervisorName}

\institution{Universitatea Tehnică a Moldovei}
\faculty{Facultatea}
\department{Departamentul}
\reporttitle{Titlul lucrării}
\reportsubtitle{}
\student{Prenumele NUMELE}{grupa}
\supervisor{Prenumele NUMELE}{titlul științific}
\consultant{}{}
\cityyear{Chișinău}{2026}
\documenttype{Raport}
\studyprogramme{}
\team{Team number or name}
\submissiondate{day / month / year}
\internshipmentor{Mentor name from internship contract}

\newcommand{\UTMReportInstitutionBlock}{%
  \noindent\begin{minipage}[c]{0.20\textwidth}
    \centering\includegraphics[width=0.88\linewidth]{\UTMLogoPath}
  \end{minipage}\hfill
  \begin{minipage}[c]{0.77\textwidth}
    \centering\linespread{1}\selectfont
    {\bfseries\fontsize{14pt}{17pt}\selectfont\MakeUppercase{\UTMInstitution}\par}
    {\fontsize{13pt}{17pt}\selectfont\UTMFaculty\par\UTMDepartment\par
    \ifx\UTMStudyProgramme\empty\else\UTMStudyProgramme\par\fi}
  \end{minipage}\par
}

\newcommand{\makeutmcover}{%
  \begin{titlepage}
  \centering
  \includegraphics[height=2cm]{\UTMLogoPath}\par\vspace{0.5cm}
  {\bfseries\fontsize{16pt}{18pt}\selectfont \UTMInstitution\par}
  \vspace{4cm}
  {\bfseries\fontsize{22pt}{26pt}\selectfont \UTMReportTitle\par}
  \ifx\UTMReportSubtitle\empty\else
    \vspace{0.5cm}{\bfseries\fontsize{16pt}{18pt}\selectfont \UTMReportSubtitle\par}
  \fi
  \vspace{3cm}
  \hfill\begin{minipage}{0.65\textwidth}
  \raggedright\bfseries\fontsize{16pt}{18pt}\selectfont
  Student: \UTMStudentName\\
  Grupa: \UTMStudentGroup\\[0.5cm]
  Conducător: \UTMSupervisorTitle, \UTMSupervisorName
  \end{minipage}
  \vfill
  {\bfseries\fontsize{16pt}{18pt}\selectfont \UTMCity{} - \UTMYear\par}
  \end{titlepage}
}

\newcommand{\makeutmreporttitlepage}{%
  \begin{titlepage}
  \centering
  \UTMReportInstitutionBlock
  \vspace{3cm}
  {\bfseries\fontsize{20pt}{24pt}\selectfont\MakeUppercase{\UTMDocumentType}\par}
  \vspace{0.6cm}
  {\bfseries\fontsize{18pt}{22pt}\selectfont\UTMReportTitle\par}
  \ifx\UTMReportSubtitle\empty\else
    \vspace{0.6cm}{\bfseries\fontsize{14pt}{17pt}\selectfont \UTMReportSubtitle\par}
  \fi
  \vspace{2.5cm}
  \hfill\begin{minipage}{0.62\textwidth}
  \raggedleft\fontsize{12pt}{14pt}\selectfont
  \textbf{\UTMStudentLabel:} \UTMStudentName\\
  \textbf{\UTMGroupLabel:} \UTMStudentGroup\\[4pt]
  \textbf{\UTMCoordinatorLabel:} \UTMSupervisorDisplay
  \end{minipage}
  \vfill
  {\bfseries\fontsize{12pt}{14pt}\selectfont \UTMCity{} - \UTMYear\par}
  \end{titlepage}
  \setcounter{page}{2}\utmfrontmatter
}

\newcommand{\makeutminternshiptitlepage}{%
  \begin{titlepage}\centering
  \UTMReportInstitutionBlock
  \vspace{3cm}
  {\bfseries\fontsize{20pt}{24pt}\selectfont\MakeUppercase{\UTMDocumentType}\par}
  \vspace{0.6cm}
  {\bfseries\fontsize{18pt}{22pt}\selectfont\UTMReportTitle\par}
  \vspace{1.6cm}\UTMTeam\par
  \vspace{0.8cm}\noindent\textbf{Team members:}\par\vspace{0.3cm}
  {\renewcommand{\arraystretch}{1.25}
  \begin{tabularx}{\textwidth}{|p{0.30\textwidth}|>{\raggedleft\arraybackslash}X|p{0.18\textwidth}|}\hline
  \UTMTeamRows
  \textbf{University Internship Mentor} & \UTMInternshipMentor & \rule{1.2cm}{0.3pt}\\\hline
  \end{tabularx}\par}
  \vspace{0.5cm}
  {\fontsize{10pt}{12pt}\selectfont\itshape\justifying
  By signing the cover page of this report, each student confirms their active involvement in both the development of the project and the writing of the report. Students who did not contribute meaningfully are not permitted to sign.\par}
  \vfill Submission date: \UTMSubmissionDate\par
  \vspace{1cm}{\bfseries\UTMCity, \UTMYear\par}
  \end{titlepage}\setcounter{page}{2}\utmfrontmatter
}

\newcommand{\makeutmthesistitlepage}{%
  \begin{titlepage}
  \centering
  {\bfseries\fontsize{12pt}{14pt}\selectfont
  Ministerul Educației și Cercetării al Republicii Moldova\\
  \UTMInstitution\\
  \UTMFaculty\\
  \UTMDepartment\par}
  \vspace{1.5cm}
  \hfill\begin{minipage}{0.45\textwidth}
  \raggedright\bfseries\fontsize{11pt}{13pt}\selectfont
  Admis la susținere\\
  Șef departament: \rule{4.5cm}{0.4pt}\\
  ``\rule{0.8cm}{0.4pt}''\rule{2.5cm}{0.4pt} \UTMYear
  \end{minipage}
  \vspace{2cm}
  {\bfseries\fontsize{20pt}{24pt}\selectfont \UTMReportTitle\par}
  \vspace{0.7cm}
  {\bfseries\fontsize{16pt}{19pt}\selectfont \UTMDocumentType\par}
  \vspace{2cm}
  \hfill\begin{minipage}{0.72\textwidth}
  \raggedright\bfseries\fontsize{12pt}{14pt}\selectfont
  Student: \rule{2.4cm}{0.4pt} \UTMStudentName, \UTMStudentGroup\\[4pt]
  Conducător: \rule{2.4cm}{0.4pt} \UTMSupervisorTitle, \UTMSupervisorName\\[4pt]
  \ifx\UTMConsultantName\empty\else Consultant: \rule{2.4cm}{0.4pt} \UTMConsultantTitle, \UTMConsultantName\\[4pt]\fi
  \end{minipage}
  \vfill
  {\bfseries\fontsize{12pt}{14pt}\selectfont \UTMCity{} - \UTMYear\par}
  \end{titlepage}
}

\newenvironment{utmabstract}[1][Rezumat]
  {\chapter*{#1}\addcontentsline{toc}{chapter}{#1}}
  {\clearpage}

\newcommand{\utmkeywords}[1]{\noindent\textbf{\iflanguage{english}{Keywords}{Cuvinte cheie}:} #1\par}
