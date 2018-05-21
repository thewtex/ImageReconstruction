/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

#ifndef itkMontageTestHelper_hxx
#define itkMontageTestHelper_hxx

#include "itkTileMontage.h"
#include "itkMaxPhaseCorrelationOptimizer.h"
#include "itkImageFileReader.h"
#include "itkImageFileWriter.h"
#include "itkSimpleFilterWatcher.h"

#include <type_traits>
#include <array>
#include <fstream>
#include <iomanip>

 //do the registrations and calculate registration errors
template<typename PixelType, unsigned xMontageSize, unsigned yMontageSize, typename PositionTableType, typename FilenameTableType>
int montageTest(const PositionTableType& stageCoords, const PositionTableType& actualCoords,
    const FilenameTableType& filenames, const std::string& outFilename, bool varyPaddingMethods)
{
  int result = EXIT_SUCCESS;
  constexpr unsigned Dimension = 2;
  using PointType = itk::Point<double, Dimension>;
  using VectorType = itk::Vector<double, Dimension>;
  using TransformType = itk::TranslationTransform<double, Dimension>;
  using ImageType = itk::Image< PixelType, Dimension>;
  using ImageTypePointer = typename ImageType::Pointer;
  using PCMType = itk::PhaseCorrelationImageRegistrationMethod<ImageType, ImageType>;
  using PadMethodUnderlying = typename std::underlying_type<typename PCMType::PaddingMethod>::type;
  typename ImageType::SpacingType sp;
  sp.Fill(1.0); //OMC test assumes unit spacing, tiles test has explicit unit spacing

  using ReaderType = itk::ImageFileReader< ImageType >;
  typename ReaderType::Pointer reader = ReaderType::New();

  using ImageTableType = std::array<std::array<ImageTypePointer, xMontageSize>, yMontageSize>;
  ImageTableType imageTable;

  for (unsigned y = 0; y < yMontageSize; y++)
    {
    for (unsigned x = 0; x < xMontageSize; x++)
      {
      reader->SetFileName(filenames[y][x]);
      reader->Update();
      imageTable[y][x] = reader->GetOutput();
      imageTable[y][x]->DisconnectPipeline();
      imageTable[y][x]->SetOrigin(stageCoords[y][x]);
      imageTable[y][x]->SetSpacing(sp);
      }
    }

  using PeakInterpolationType = typename itk::MaxPhaseCorrelationOptimizer< PCMType >::PeakInterpolationMethod;
  using PeakFinderUnderlying = typename std::underlying_type<PeakInterpolationType>::type;

  for (auto padMethod = static_cast<PadMethodUnderlying>(PCMType::PaddingMethod::Zero);
      padMethod <= static_cast<PadMethodUnderlying>(PCMType::PaddingMethod::Last);
      padMethod++)
    {
    std::ofstream registrationErrors(outFilename + std::to_string(padMethod) + ".tsv");
    std::cout << "Padding method " << padMethod << std::endl;
    registrationErrors << "PeakInterpolationMethod";
    for (unsigned d = 0; d < Dimension; d++)
      {
      registrationErrors << '\t' << char('x' + d) << "Tile";
      }
    for (unsigned d = 0; d < Dimension; d++)
      {
      registrationErrors << '\t' << char('x' + d) << "Error";
      }
    registrationErrors << std::endl;


    using MontageType = itk::TileMontage<ImageType>;
    typename MontageType::Pointer montage = MontageType::New();
    montage->SetMontageSize({ xMontageSize, yMontageSize });

    typename MontageType::TileIndexType ind;
    for (unsigned y = 0; y < yMontageSize; y++)
      {
      ind[1] = y;
      for (unsigned x = 0; x < xMontageSize; x++)
        {
        ind[0] = x;
        montage->SetInputTile(ind, imageTable[y][x]);
        }
      }

    for (auto peakMethod = static_cast<PeakFinderUnderlying>(PeakInterpolationType::None);
      peakMethod <= static_cast<PeakFinderUnderlying>(PeakInterpolationType::Last);
      peakMethod++)
      {
      montage->GetModifiablePCMOptimizer()->SetPeakInterpolationMethod(static_cast<PeakInterpolationType>(peakMethod));
      montage->Modified(); // optimizer is not an "input" to PCM
      //so its modification does not cause a pipeline update automatically
      
      std::cout << "    PeakMethod " << peakMethod << std::endl;
      itk::SimpleFilterWatcher fw(montage);
      montage->Update();

      std::cout << std::fixed;

      double totalError = 0.0;
      for (unsigned y = 0; y < yMontageSize; y++)
        {
        ind[1] = y;
        for (unsigned x = 0; x < xMontageSize; x++)
          {
          ind[0] = x;
          const TransformType* regTr = montage->GetOutputTransform(ind);

          registrationErrors << peakMethod << '\t' << x << '\t' << y;

          //calculate error
          VectorType tr = regTr->GetOffset(); //translation measured by registration
          VectorType ta = stageCoords[y][x] - actualCoords[y][x]; //translation (actual)
          PointType p0;
          p0.Fill(0);
          ta += actualCoords[0][0] - p0; //account for tile zero maybe not being at coordinates 0
          for (unsigned d = 0; d < Dimension; d++)
            {
            registrationErrors << '\t' << (tr[d] - ta[d]);
            std::cout << "  " << std::setw(8) << std::setprecision(3) << (tr[d] - ta[d]);
            totalError += std::abs(tr[d] - ta[d]);
            }
          registrationErrors << std::endl;
          }
        }
      double avgError = totalError / (xMontageSize*(yMontageSize - 1) + (xMontageSize - 1)*yMontageSize);
      avgError /= Dimension; //report per-dimension error
      std::cout << "\nAverage translation error for padding method " << padMethod
          << " and peak interpolation method " << peakMethod << ": " << avgError << std::endl;
      if (avgError >= 1.0)
        {
        result = EXIT_FAILURE;
        }
      // write generated mosaic
      ImageTypePointer image = montage->ResampleIntoSingleImage(false);
      using WriterType = itk::ImageFileWriter<ImageType>;
      typename WriterType::Pointer w = WriterType::New();
      w->SetInput(image);
      w->SetFileName(outFilename + std::to_string(padMethod) + "_" + std::to_string(peakMethod) + ".nrrd");
      //w->UseCompressionOn();
      w->Update();
      }

    if (!varyPaddingMethods)
      {
      break;
      }
    std::cout << std::endl;
    }
  return result;
}

#endif //itkMontageTestHelper_hxx
