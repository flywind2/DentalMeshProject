#include "ToothSegmentation.h"
#include "Mesh.h"

#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

#include <igl/read_triangle_mesh.h>
#include <igl/cotmatrix.h>
#include <igl/massmatrix.h>
#include <igl/invert_diag.h>
#include <igl/principal_curvature.h>

#include <QProgressDialog>
#include <QTime>
#include <QMessageBox>
#include <QFile>

#include <math.h>

#include <gsl/gsl_spline.h>

#include <pcl/point_cloud.h>
#include <pcl/kdtree/kdtree_flann.h>

using namespace SW;
using namespace std;
using namespace Eigen;

const string ToothSegmentation::mVPropHandleCurvatureName = "vprop_curvature";
const string ToothSegmentation::mVPropHandleCurvatureComputedName = "vprop_curvature_computed";
const string ToothSegmentation::mVPropHandleIsToothBoundaryName = "vprop_is_tooth_boundary";
const string ToothSegmentation::mVPropHandleBoundaryVertexTypeName = "vprop_boundary_vertex_type";
const string ToothSegmentation::mVPropHandleNonBoundaryRegionTypeName = "vprop_non_boundary_region_type";
const string ToothSegmentation::mVPropHandleRegionGrowingVisitedName = "vprop_region_growing_visited";
const string ToothSegmentation::mVPropHandleBoundaryTypeName = "vprop_boundary_type";
const string ToothSegmentation::mVPropHandleSearchContourSectionVisitedName = "vprop_search_contour_section_visited";

ToothSegmentation::ToothSegmentation(QWidget *parentWidget, const Mesh &toothMesh)
{
    mParentWidget = parentWidget;
    setToothMesh(toothMesh);
}

void ToothSegmentation::setToothMesh(const Mesh &toothMesh)
{
    mToothMesh = toothMesh;

    //在Mesh添加自定义属性
    if(!mToothMesh.get_property_handle(mVPropHandleCurvature, mVPropHandleCurvatureName))
    {
        mToothMesh.add_property(mVPropHandleCurvature, mVPropHandleCurvatureName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleCurvatureComputed, mVPropHandleCurvatureComputedName))
    {
        mToothMesh.add_property(mVPropHandleCurvatureComputed, mVPropHandleCurvatureComputedName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleIsToothBoundary, mVPropHandleIsToothBoundaryName))
    {
        mToothMesh.add_property(mVPropHandleIsToothBoundary, mVPropHandleIsToothBoundaryName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleBoundaryVertexType, mVPropHandleBoundaryVertexTypeName))
    {
        mToothMesh.add_property(mVPropHandleBoundaryVertexType, mVPropHandleBoundaryVertexTypeName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleNonBoundaryRegionType, mVPropHandleNonBoundaryRegionTypeName))
    {
        mToothMesh.add_property(mVPropHandleNonBoundaryRegionType, mVPropHandleNonBoundaryRegionTypeName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleRegionGrowingVisited, mVPropHandleRegionGrowingVisitedName))
    {
        mToothMesh.add_property(mVPropHandleRegionGrowingVisited, mVPropHandleRegionGrowingVisitedName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleBoundaryType, mVPropHandleBoundaryTypeName))
    {
        mToothMesh.add_property(mVPropHandleBoundaryType, mVPropHandleBoundaryTypeName);
    }
    if(!mToothMesh.get_property_handle(mVPropHandleSearchContourSectionVisited, mVPropHandleSearchContourSectionVisitedName))
    {
        mToothMesh.add_property(mVPropHandleSearchContourSectionVisited, mVPropHandleSearchContourSectionVisitedName);
    }

    //如果Mesh中没有顶点颜色这个属性，则添加之
    if(!mToothMesh.has_vertex_colors())
    {
        mToothMesh.request_vertex_colors();
    }
}

Mesh ToothSegmentation::getToothMesh() const
{
    return mToothMesh;
}

Mesh ToothSegmentation::getExtraMesh() const
{
    return mExtraMesh;
}

void ToothSegmentation::identifyPotentialToothBoundary(bool loadStateFromFile)
{
    QProgressDialog progress(mParentWidget);
    progress.setWindowTitle("Identify potential tooth boundary...");
    progress.setMinimumSize(400, 120);
    progress.setCancelButtonText("cancel");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    progress.setAutoClose(false);

    //如果存在之前保存的状态，则读取之
    if(loadStateFromFile && loadState(progress, "IdentifyPotentialToothBoundary"))
    {
        return;
    }

    identifyPotentialToothBoundary(progress);

    paintAllVerticesWhite(progress);
    paintBoundaryVertices(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".IdentifyPotentialToothBoundary.off");

    //测试，将曲率值转换成伪彩色显示在模型上
    //curvature2PseudoColor();
    //saveToothMesh(mToothMesh.MeshName.toStdString() + ".IdentifyPotentialToothBoundary.ShowCurvatureByPseudoColor.off");

    //保存当前状态（节省调试时间）
    saveState(progress, "IdentifyPotentialToothBoundary");

    //关闭进度条
    progress.close();
}

void ToothSegmentation::identifyPotentialToothBoundary(QProgressDialog &progress)
{
    //计算顶点处曲率
    computeCurvature(progress);

    //计算曲率阈值
    double curvatureMin, curvatureMax;
    computeCurvatureMinAndMax(curvatureMin, curvatureMax);
    double curvatureThreshold = curvatureMin * 0.01; //TODO 经肉眼观察，对于模型36293X_Zhenkan_070404.obj，0.01这个值最合适。

    //根据曲率阈值判断初始边界点
    mBoundaryVertexNum = 0;
    int vertexIndex = 0;
    progress.setLabelText("Finding boundary by curvature...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        if(!mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter)) //跳过未被正确计算出曲率的顶点
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = false;
            continue;
        }
        if(mToothMesh.property(mVPropHandleCurvature, *vertexIter) < curvatureThreshold) //如果该顶点处的曲率小于某个阈值，则确定为初始边界点
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = true;
            mBoundaryVertexNum++;
        }
        else
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = false;
        }
        vertexIndex++;
    }

    //测试，显示边界点数目
    //QMessageBox::information(mParentWidget, "Info", QString("Boundary vertices: %1\nAll vertices: %2").arg(mBoundaryVertexNum).arg(mToothMesh.mVertexNum));

    //形态学操作
    dilateBoundary(progress);
    dilateBoundary(progress);
    dilateBoundary(progress);
    //corrodeBoundary(progress);
    //corrodeBoundary(progress);
    dilateBoundary(progress);
    dilateBoundary(progress);
    dilateBoundary(progress);

    //测试，显示形态学操作后边界点数目
    //QMessageBox::information(mParentWidget, "Info", QString("Boundary vertices: %1\nAll vertices: %2").arg(mBoundaryVertexNum).arg(mToothMesh.mVertexNum));
}

void ToothSegmentation::computeCurvature(QProgressDialog &progress)
{
    QTime time;
    time.start();

    VectorXd H2(mToothMesh.mVertexNum); //平均曲率
    vector<bool> currentCurvatureComputed(mToothMesh.mVertexNum); //记录每个顶点是否被正确计算得到曲率

    //如果之前将曲率信息保存到文件，则从文件中读取之，否则重新计算
    string curvatureFileName = mToothMesh.MeshName.toStdString() + ".curvature";
    QFile curvatureFile(curvatureFileName.c_str());
    if(curvatureFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::information(mParentWidget, "Info", "Curvature file found!\nCurvature data will be loaded from it.");
        time.start();
        bool tempCurvatureComputed;
        for(int vertexIndex = 0; vertexIndex < mToothMesh.mVertexNum; vertexIndex++)
        {
            curvatureFile.read((char *)(&(H2(vertexIndex))), sizeof(double));
            curvatureFile.read((char *)(&tempCurvatureComputed), sizeof(bool));
            currentCurvatureComputed[vertexIndex] = tempCurvatureComputed;
        }
        curvatureFile.close();
        cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "从二进制文件中读取曲率信息" << " ended." << endl;
    }
    else
    {
        //从Mesh类中提取用于libIGL库计算的Eigen格式的网格顶点列表和面片列表
        MatrixXd V(mToothMesh.mVertexNum, 3); //网格顶点列表
        MatrixXi F(mToothMesh.mFaceNum, 3); //网格三角片面列表
        Mesh::Point tempVertex;
        int vertexIndex = 0, vertexCoordinateIndex;
        progress.setLabelText("Extracting vertex list...");
        progress.setMinimum(0);
        progress.setMaximum(mToothMesh.mVertexNum);
        progress.setValue(0);
        for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
        {
            progress.setValue(vertexIndex);
            tempVertex = mToothMesh.point(*vertexIter);
            for(vertexCoordinateIndex = 0; vertexCoordinateIndex < 3; vertexCoordinateIndex++)
            {
                V(vertexIndex, vertexCoordinateIndex) = tempVertex[vertexCoordinateIndex];
            }
            vertexIndex++;
        }
        cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "提取网格顶点列表" << " ended." << endl;
        int faceIndex = 0, faceVertexIndex;
        progress.setLabelText("Extracting face list...");
        progress.setMinimum(0);
        progress.setMaximum(mToothMesh.mFaceNum);
        progress.setValue(0);
        for(Mesh::FaceIter faceIter = mToothMesh.faces_begin(); faceIter != mToothMesh.faces_end(); faceIter++)
        {
            progress.setValue(faceIndex);
            faceVertexIndex = 0;
            for(Mesh::FaceVertexIter faceVertexIter = mToothMesh.fv_iter(*faceIter); faceVertexIter.is_valid(); faceVertexIter++)
            {
                F(faceIndex, faceVertexIndex) = faceVertexIter->idx();
                faceVertexIndex++;
            }
            faceIndex++;
        }
        cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "提取网格顶点列表" << " ended." << endl;

        //带进度条的计算平均曲率
        MatrixXd PD1, PD2;
        VectorXd PV1, PV2;
        progress.setLabelText("Computing curvature...");
        progress.setMinimum(0);
        progress.setMaximum(mToothMesh.mVertexNum);
        progress.setValue(0);
        igl::principal_curvature(V, F, PD1, PD2, PV1, PV2, currentCurvatureComputed, progress); // Compute curvature directions via quadric fitting
        H2 = 0.5 * (PV1 + PV2); // mean curvature
        cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "计算平均曲率" << " ended." << endl;

        //显示计算曲率出错点数量
        int curvatureComputeFailedNum = 0;
        for(vertexIndex = 0; vertexIndex < mToothMesh.mVertexNum; vertexIndex++)
        {
            if(!currentCurvatureComputed[vertexIndex])
            {
                curvatureComputeFailedNum++;
            }
        }
        cout << "Compute curvature finished!\n" << curvatureComputeFailedNum << "/" << mToothMesh.mVertexNum << "vertices failed." << endl;

        //将曲率信息保存到二进制文件
        if(curvatureFile.open(QIODevice::WriteOnly))
        {
            bool tempCurvatureComputed;
            for(int vertexIndex = 0; vertexIndex < mToothMesh.mVertexNum; vertexIndex++)
            {
                curvatureFile.write((char *)(&(H2(vertexIndex))), sizeof(double));
                tempCurvatureComputed = currentCurvatureComputed[vertexIndex];
                curvatureFile.write((char *)(&tempCurvatureComputed), sizeof(bool));
            }
            curvatureFile.close();
        }
        else
        {
            cerr << "Fail to open file \"" << curvatureFileName << "\" ." << endl;
        }
        cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "将曲率信息保存到二进制文件" << " ended." << endl;
    }

    //将计算得到的曲率信息写入到Mesh
    int vertexIndex = 0;
    progress.setLabelText("Adding curvature to mesh...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter) = currentCurvatureComputed[vertexIndex];
        mToothMesh.property(mVPropHandleCurvature, *vertexIter) = H2(vertexIndex); //可通过curvature_computed判断该顶点处曲率是否已被正确计算
        vertexIndex++;
    }
    cout << "Time elapsed " << time.elapsed() / 1000 << "s. " << "将曲率信息写入到Mesh" << " ended." << endl;
}

void ToothSegmentation::curvature2PseudoColor()
{
    //计算曲率的最大值和最小值
    double curvatureMin, curvatureMax;
    computeCurvatureMinAndMax(curvatureMin, curvatureMax);
    cout << "curvatureMin: " << curvatureMin << ", curvatureMax: " << curvatureMax << endl;

    //计算对应的伪彩色
    curvatureMin /= 8.0; curvatureMax /= 8.0;
    Mesh::Color colorPseudoRGB;
    float colorGray;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter)) //将未被正确计算出曲率的顶点颜色设置为紫色（因为伪彩色中没有紫色）
        {
            colorPseudoRGB[0] = 1.0;
            colorPseudoRGB[1] = 0.0;
            colorPseudoRGB[2] = 1.0;
        }
        else
        {
            colorGray = (mToothMesh.property(mVPropHandleCurvature, *vertexIter) - curvatureMin) / (curvatureMax - curvatureMin);
            //colorGray = (mToothMesh.property(mVPropHandleCurvature, *vertexIter) + 8.0) / 16.0;
            gray2PseudoColor(colorGray, colorPseudoRGB);
        }
        mToothMesh.set_color(*vertexIter, colorPseudoRGB);
    }
}

void ToothSegmentation::computeCurvatureMinAndMax(double &curvatureMin, double &curvatureMax)
{
    curvatureMin = 1000000.0; //TODO 初始化最小值为某个足够大的值（因为第一个顶点不确定是否被正确计算出曲率）
    curvatureMax = -1000000.0;
    double tempCurvature;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter)) //跳过未被正确计算出曲率的顶点
        {
            continue;
        }
        tempCurvature = mToothMesh.property(mVPropHandleCurvature, *vertexIter);
        if(tempCurvature > curvatureMax)
        {
            curvatureMax = tempCurvature;
        }
        else if(tempCurvature < curvatureMin)
        {
            curvatureMin = tempCurvature;
        }
    }
}

void ToothSegmentation::corrodeBoundary(QProgressDialog &progress)
{
    int neighborNotBoundaryVertexNum; //邻域中非边界点的个数
    int boundaryVertexIndex = 0;
    bool *boundaryVertexEliminated = new bool[mBoundaryVertexNum]; //标记对应边界点是否应被剔除
    progress.setLabelText("Corroding boundary...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum * 2);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点（包括未被正确计算出曲率的点，因为在上一步根据曲率阈值确定初始边界的过程中，未被正确计算出曲率的点全部被标记为非初始边界点）
        {
            continue;
        }
        progress.setValue(boundaryVertexIndex);
        //计算邻域中非边界点的个数
        neighborNotBoundaryVertexNum = 0;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(*vertexIter); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
            {
                neighborNotBoundaryVertexNum++;
            }
        }
        //邻域中非边界点的个数大于某值的标记剔除
        boundaryVertexEliminated[boundaryVertexIndex] = (neighborNotBoundaryVertexNum > 2);
        boundaryVertexIndex++;
    }
    boundaryVertexIndex = 0;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点（包括未被正确计算出曲率的点，因为在上一步根据曲率阈值确定初始边界的过程中，未被正确计算出曲率的点全部被标记为非初始边界点）
        {
            continue;
        }
        progress.setValue(mBoundaryVertexNum + boundaryVertexIndex);
        //剔除被标记为应删除的边界点
        if(boundaryVertexEliminated[boundaryVertexIndex])
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = false;
            mBoundaryVertexNum--;
        }
        boundaryVertexIndex++;
    }
    delete []boundaryVertexEliminated;
}

void ToothSegmentation::dilateBoundary(QProgressDialog &progress)
{
    int neighborBoundaryVertexNum; //邻域中边界点的个数
    int notBoundaryVertexIndex = 0;
    bool *boundaryVertexAdded = new bool[mToothMesh.mVertexNum - mBoundaryVertexNum]; //标记对应非边界点是否应被添加为边界点
    progress.setLabelText("Dilating boundary...");
    progress.setMinimum(0);
    progress.setMaximum((mToothMesh.mVertexNum - mBoundaryVertexNum) * 2);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过初始边界点
        {
            continue;
        }
        progress.setValue(notBoundaryVertexIndex);
        //计算邻域中边界点的个数
        neighborBoundaryVertexNum = 0;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(*vertexIter); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
            {
                neighborBoundaryVertexNum++;
            }
        }
        //邻域中边界点的个数大于某值的标记添加
        boundaryVertexAdded[notBoundaryVertexIndex] = (neighborBoundaryVertexNum > 2);
        notBoundaryVertexIndex++;
    }
    notBoundaryVertexIndex = 0;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过初始边界点
        {
            continue;
        }
        progress.setValue((mToothMesh.mVertexNum - mBoundaryVertexNum) + notBoundaryVertexIndex);
        //添加被标记为应添加的非边界点
        if(boundaryVertexAdded[notBoundaryVertexIndex])
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = true;
            mBoundaryVertexNum++;
        }
        notBoundaryVertexIndex++;
    }
    delete []boundaryVertexAdded;
}

void ToothSegmentation::paintBoundaryVertices(QProgressDialog &progress)
{
    int vertexIndex = 0;
    Mesh::Color colorRed(1.0, 0.0, 0.0), colorWhite(1.0, 1.0, 1.0);
    progress.setLabelText("Painting boundary vertices...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter))
        {
            mToothMesh.set_color(*vertexIter, colorRed);
        }
        else
        {
            //mToothMesh.set_color(*vertexIter, colorWhite);
        }
        vertexIndex++;
    }
}

void ToothSegmentation::automaticCuttingOfGingiva(bool loadStateFromFile)
{
    QProgressDialog progress(mParentWidget);
    progress.setWindowTitle("Automatic cutting Of gingiva...");
    progress.setMinimumSize(400, 120);
    progress.setCancelButtonText("cancel");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    progress.setAutoClose(false);

    //如果存在之前保存的状态，则读取之
    if(loadStateFromFile && loadState(progress, "AutomaticCuttingOfGingiva"))
    {
        return;
    }

    automaticCuttingOfGingiva(progress);

    paintClassifiedNonBoundaryRegions(progress);
    paintBoundaryVertices(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".AutomaticCuttingOfGingiva.off");

    //创建牙龈分割平面mesh
    mToothMesh.computeBoundingBox();
    double boundingBoxMaxEdgeLength = mToothMesh.BBox.size.x; //BoundingBox的最大边长
    if(mToothMesh.BBox.size.y > boundingBoxMaxEdgeLength)
    {
        boundingBoxMaxEdgeLength = mToothMesh.BBox.size.y;
    }
    if(mToothMesh.BBox.size.z > boundingBoxMaxEdgeLength)
    {
        boundingBoxMaxEdgeLength = mToothMesh.BBox.size.z;
    }
    float gingivaCuttingPlaneSize = boundingBoxMaxEdgeLength * 1.5; //TODO 1.5的意义是使画出来的分割平面比模型稍大一点
    createPlaneInExtraMesh(mGingivaCuttingPlanePoint, mGingivaCuttingPlaneNormal, gingivaCuttingPlaneSize);
    saveExtraMesh(mToothMesh.MeshName.toStdString() + ".AutomaticCuttingOfGingiva.Extra.GingivaCuttingPlane.off");

    //保存当前状态
    saveState(progress, "AutomaticCuttingOfGingiva");

    //关闭进度条
    progress.close();
}

void ToothSegmentation::automaticCuttingOfGingiva(QProgressDialog &progress)
{
    //计算初始边界点质心
    mGingivaCuttingPlanePoint = Mesh::Point(0.0, 0.0, 0.0); //质心点
    float tempCurvature, curvatureSum = 0;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            continue;
        }
        tempCurvature = mToothMesh.property(mVPropHandleCurvature, *vertexIter);
        mGingivaCuttingPlanePoint += mToothMesh.point(*vertexIter) * tempCurvature; //将该点曲率作为加权
        curvatureSum += tempCurvature;
    }
    mGingivaCuttingPlanePoint /= curvatureSum;

    //测试，输出质心点
    cout << "质心点：\n" << mGingivaCuttingPlanePoint << endl;

    //计算协方差矩阵
    Matrix3f covarMat;
//    covarMat << 0, 0, 0,
//            0, 0, 0,
//            0, 0, 0; //还可以用covarMat=MatrixXf::Zero(3, 3);或covarMat.setZero();实现初始化为0
    covarMat.setZero(3, 3);
    Matrix3f tempMat;
    Mesh::Point tempVertex;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            continue;
        }
        tempVertex = mToothMesh.point(*vertexIter);
        tempVertex -= mGingivaCuttingPlanePoint;
        tempMat << tempVertex[0] * tempVertex[0], tempVertex[0] * tempVertex[1], tempVertex[0] * tempVertex[2],
                tempVertex[1] * tempVertex[0], tempVertex[1] * tempVertex[1], tempVertex[1] * tempVertex[2],
                tempVertex[2] * tempVertex[0], tempVertex[2] * tempVertex[1], tempVertex[2] * tempVertex[2];
        covarMat += tempMat;
    }
    covarMat /= mBoundaryVertexNum;

    //测试，输出协方差矩阵
    cout << "协方差矩阵：\n" << covarMat << endl;

    //计算分割平面法向量（即协方差矩阵对应最小特征值的特征向量）
    EigenSolver<Matrix3f> eigenSolver(covarMat, true);
    EigenSolver<Matrix3f>::EigenvalueType eigenvalues = eigenSolver.eigenvalues();
    EigenSolver<Matrix3f>::EigenvectorsType eigenvectors = eigenSolver.eigenvectors();
    float eigenvalueMin = eigenvalues(0).real();
    int eigenvalueMinIndex = 0;
    for(int i = 1; i < 3; i++)
    {
        if(eigenvalues(i).real() < eigenvalueMin)
        {
            eigenvalueMin = eigenvalues(i).real();
            eigenvalueMinIndex = i;
        }
    }
    //测试，输出所有特征值和特征向量
    cout << "所有特征值：\n" << eigenvalues << endl;
    cout << "所有特征向量：\n" << eigenvectors << endl;
    mGingivaCuttingPlaneNormal = -Mesh::Normal(eigenvectors(0, eigenvalueMinIndex).real(), eigenvectors(1, eigenvalueMinIndex).real(), eigenvectors(2, eigenvalueMinIndex).real()); //eigenvectors的每一列为一个特征向量
    //mGingivaCuttingPlaneNormal = Mesh::Normal(0.0, 1.0, 0.0); //假设的值

    //手动调整（沿法向量方向平移）牙龈分割平面，TODO 需要区分牙齿上颚和下颚
    double boundingBoxMinEdgeLength = mToothMesh.BBox.size.x; //BoundingBox的最小边长
    if(mToothMesh.BBox.size.y < boundingBoxMinEdgeLength)
    {
        boundingBoxMinEdgeLength = mToothMesh.BBox.size.y;
    }
    if(mToothMesh.BBox.size.z < boundingBoxMinEdgeLength)
    {
        boundingBoxMinEdgeLength = mToothMesh.BBox.size.z;
    }
    mGingivaCuttingPlanePoint += mGingivaCuttingPlaneNormal * boundingBoxMinEdgeLength * 0.2; //TODO 此处平移距离有待自动化计算

    //剔除牙龈上的初始边界点
    removeBoundaryVertexOnGingiva(progress);

    //标记非边界区域
    markNonBoundaryRegion(progress);
}

void ToothSegmentation::boundarySkeletonExtraction(bool loadStateFromFile)
{
    QProgressDialog progress(mParentWidget);
    progress.setWindowTitle("Boundary skeleton extraction...");
    progress.setMinimumSize(400, 120);
    progress.setCancelButtonText("cancel");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    progress.setAutoClose(false);

    //如果存在之前保存的状态，则读取之
    if(loadStateFromFile && loadState(progress, "BoundarySkeletonExtraction"))
    {
        return;
    }

    boundarySkeletonExtraction(progress);

    //测试，将迭代后剩下的非单点宽度边界点（由于算法bug导致）保存到文件
    /*mExtraMesh = Mesh();
    if(!mExtraMesh.has_vertex_colors())
    {
        mExtraMesh.request_vertex_colors();
    }
    Mesh::VertexHandle tempVertexHandle;
    Mesh::Color colorBlue(0.0, 0.0, 1.0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter))
        {
            continue;
        }
        if(mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) == COMPLEX_VERTEX)
        {
            continue;
        }
        tempVertexHandle = mExtraMesh.add_vertex(mToothMesh.point(*vertexIter));
        mExtraMesh.set_color(tempVertexHandle, colorBlue);
    }
    saveExtraMesh(mToothMesh.MeshName.toStdString() + ".BoundarySkeletonExtraction.Extra.ErrorVertices.off");*/

    paintClassifiedBoundaryVertices(progress);
    paintClassifiedNonBoundaryRegions(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".BoundarySkeletonExtraction.off");

    //保存当前状态
    saveState(progress, "BoundarySkeletonExtraction");

    //关闭进度条
    progress.close();
}

void ToothSegmentation::boundarySkeletonExtraction(QProgressDialog &progress)
{
    //逐步删除某一类外围点
    int *classifiedBoundaryVertexNum = new int[mToothNum + DISK_VERTEX_TOOTH];
    int deleteIterTimes = 0; //迭代次数
    bool deleteIterationFinished = false;

    //边界点分类
    classifyBoundaryVertex(progress, classifiedBoundaryVertexNum);

    int diskVertexTypeIndex, diskVertexTypeIndex2;
    int startCenterAndDiskVertexNum = 0; //迭代前内部点和外围点总数
    startCenterAndDiskVertexNum += classifiedBoundaryVertexNum[CENTER_VERTEX];
    for(diskVertexTypeIndex = 0; diskVertexTypeIndex < mToothNum + 1; diskVertexTypeIndex++)
    {
        startCenterAndDiskVertexNum += classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA + diskVertexTypeIndex];
    }
    int centerAndDiskVertexNum;

    progress.setLabelText("Deleting disk vertices...");
    progress.setMinimum(0);
    progress.setMaximum(startCenterAndDiskVertexNum);
    progress.setValue(0);
    while(true)
    {
        for(diskVertexTypeIndex = 0; diskVertexTypeIndex < mToothNum + 1; diskVertexTypeIndex++)
        {
            //如果存在此类外围点，则将所有此类外围点删除，然后重新分类
            if(classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA + diskVertexTypeIndex] != 0)
            {
                for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
                {
                    if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter))
                    {
                        continue;
                    }
                    if(mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) == (DISK_VERTEX_GINGIVA + diskVertexTypeIndex))
                    {
                        mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = false;
                        mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexIter) = (GINGIVA_REGION + diskVertexTypeIndex);
                        mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexIter) = true;
                        mBoundaryVertexNum--;
                    }
                }

                classifyBoundaryVertex(progress, classifiedBoundaryVertexNum);

                //计算内部点和外围点总数
                centerAndDiskVertexNum = 0;
                centerAndDiskVertexNum += classifiedBoundaryVertexNum[CENTER_VERTEX];
                for(diskVertexTypeIndex2 = 0; diskVertexTypeIndex2 < mToothNum + 1; diskVertexTypeIndex2++)
                {
                    centerAndDiskVertexNum += classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA + diskVertexTypeIndex2];
                }

                deleteIterTimes++;
                progress.setLabelText(QString("Deleting disk vertices...\nNo.%1 iteration.\n%2 center vertices and disk vertices left.").arg(deleteIterTimes).arg(centerAndDiskVertexNum));
                progress.setValue(startCenterAndDiskVertexNum - centerAndDiskVertexNum);

                //如果已不存在外围点，则迭代结束
                if(centerAndDiskVertexNum == 0)
                {
                    deleteIterationFinished = true;
                    break;
                }
            }
        }
        if(deleteIterationFinished)
        {
            break;
        }
    }
}

void ToothSegmentation::classifyBoundaryVertex(QProgressDialog &progress, int *classifiedBoundaryVertexNum)
{
    for(int i = 0; i < mToothNum + DISK_VERTEX_TOOTH; i++)
    {
        classifiedBoundaryVertexNum[i] = 0;
    }

    int neighborVertexTypeChangeTimes; //某边界点邻域点是否属于边界点这个属性改变（从边界点到非边界点或从非边界点到边界点）的次数
    int neighborBoundaryVertexNum; //某边界点邻域中边界点数量
    Mesh::VertexVertexIter tempVvIterBegin; //由于在遍历邻域顶点时需要使用2个迭代器，因此保存初始邻域点
    int boundaryVertexIndex = 0;
//    progress.setLabelText("Classifying boundary vertices...");
//    progress.setMinimum(0);
//    progress.setMaximum(mBoundaryVertexNum);
//    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            continue;
        }
//        progress.setValue(boundaryVertexIndex);

        //如果为mesh边界点，则判断其为DISK_VERTEX_GINGIVA
        if(mToothMesh.is_boundary(*vertexIter))
        {
            mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = DISK_VERTEX_GINGIVA;
            classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA]++;
            boundaryVertexIndex++;
            continue;
        }

        neighborVertexTypeChangeTimes = 0;
        neighborBoundaryVertexNum = 0;
        tempVvIterBegin = mToothMesh.vv_iter(*vertexIter);
        for(Mesh::VertexVertexIter vertexVertexIter = tempVvIterBegin; vertexVertexIter.is_valid(); )
        {
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter) != mToothMesh.property(mVPropHandleIsToothBoundary, *((++vertexVertexIter).is_valid() ? vertexVertexIter : tempVvIterBegin)))
            {
                neighborVertexTypeChangeTimes++;
            }
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
            {
                neighborBoundaryVertexNum++;
            }
        }

        //如果为孤立点，则任意判断其为DISK_VERTEX_GINGIVA或DISK_VERTEX_TOOTH（总之要被剔除）（注意：不能直接将其设置为非边界点，因为在删除边界点之后要更新其所属的非边界区域）
        if(neighborBoundaryVertexNum == 0)
        {
            mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = DISK_VERTEX_GINGIVA;
            classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA]++;
            boundaryVertexIndex++;
            continue;
        }

        switch(neighborVertexTypeChangeTimes)
        {
        case 0:
            mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = CENTER_VERTEX;
            classifiedBoundaryVertexNum[CENTER_VERTEX]++;
            break;
        case 2:
            mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = DISK_VERTEX_GINGIVA;
            //classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA]++;
            break;
        case 4:
        default:
            mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = COMPLEX_VERTEX;
            classifiedBoundaryVertexNum[COMPLEX_VERTEX]++;
            break;
        }
        boundaryVertexIndex++;
    }

    //将外围点分类
    boundaryVertexIndex = 0;
    int regionType;
//    progress.setLabelText("Classifying boundary vertices(Disk vertices)...");
//    progress.setMinimum(0);
//    progress.setMaximum(mBoundaryVertexNum);
//    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            continue;
        }
//        progress.setValue(boundaryVertexIndex);
        if(mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) != DISK_VERTEX_GINGIVA) //跳过非外围点
        {
            boundaryVertexIndex++;
            continue;
        }
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(*vertexIter); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过初始边界点
            {
                continue;
            }
            if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexIter) == GINGIVA_REGION)
            {
                mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = DISK_VERTEX_GINGIVA;
                classifiedBoundaryVertexNum[DISK_VERTEX_GINGIVA]++;
                break;
            }
            else
            {
                regionType = mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexIter);
                mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = regionType - TOOTH_REGION + DISK_VERTEX_TOOTH;
                classifiedBoundaryVertexNum[regionType - TOOTH_REGION + DISK_VERTEX_TOOTH]++;
                break;
            }
        }
        boundaryVertexIndex++;
    }
}

void ToothSegmentation::paintClassifiedBoundaryVertices(QProgressDialog &progress)
{
    int vertexIndex = 0;
    Mesh::Color colorWhite(1.0, 1.0, 1.0), colorGreen(0.0, 1.0, 0.0), colorKelly(0.5, 1.0, 0.0), colorOrange(1.0, 0.5, 0.0), colorRed(1.0, 0.0, 0.0);
    progress.setLabelText("Painting classified boundary vertices...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            //mToothMesh.set_color(*vertexIter, colorWhite);
        }
        else
        {
            switch(mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter))
            {
            case CENTER_VERTEX:
                mToothMesh.set_color(*vertexIter, colorGreen);
                break;
            case COMPLEX_VERTEX:
                mToothMesh.set_color(*vertexIter, colorRed);
                break;
            case DISK_VERTEX_GINGIVA:
                mToothMesh.set_color(*vertexIter, colorKelly);
                break;
            case DISK_VERTEX_TOOTH:
            default:
                mToothMesh.set_color(*vertexIter, colorOrange);
                break;
            }
        }
        vertexIndex++;
    }
}

void ToothSegmentation::removeBoundaryVertexOnGingiva(QProgressDialog &progress)
{
    float x0, y0, z0; //牙龈分割平面中心点
    x0 = mGingivaCuttingPlanePoint[0];
    y0 = mGingivaCuttingPlanePoint[1];
    z0 = mGingivaCuttingPlanePoint[2];
    float x1, y1, z1; //牙龈分割平面法向量
    x1 = mGingivaCuttingPlaneNormal[0];
    y1 = mGingivaCuttingPlaneNormal[1];
    z1 = mGingivaCuttingPlaneNormal[2];

    int boundaryVertexIndex = 0;
    Mesh::Point tempBoundaryVertex;
    progress.setLabelText("Removing boundary vertices on gingiva...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非初始边界点
        {
            continue;
        }
        progress.setValue(boundaryVertexIndex);
        tempBoundaryVertex = mToothMesh.point(*vertexIter);
        //如果该初始边界点位于牙龈分割平面的上方（牙龈方向），则剔除此边界点，TODO 需要分别考虑牙齿上颚和下颚
        if(x1 * (tempBoundaryVertex[0] - x0) + y1 * (tempBoundaryVertex[1] - y0) + z1 * (tempBoundaryVertex[2] - z0) > 0)
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = false;
            mBoundaryVertexNum--;
        }
        boundaryVertexIndex++;
    }
}

void ToothSegmentation::markNonBoundaryRegion(QProgressDialog &progress)
{
    //初始化所有非边界点的NonBoundaryRegionType属性为TOOTH_REGION，RegionGrowingVisited属性为false
    int vertexIndex = 0;
    progress.setLabelText("Init NonBoundaryRegionType of all nonboundary vertices...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexIter) = TOOTH_REGION;
        mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexIter) = false;
        vertexIndex++;
    }

    //选择牙龈区域生长种子点
    Mesh::Point tempVertex;
    Mesh::VertexIter vertexIter;
    float x0, y0, z0; //牙龈分割平面中心点
    x0 = mGingivaCuttingPlanePoint[0];
    y0 = mGingivaCuttingPlanePoint[1];
    z0 = mGingivaCuttingPlanePoint[2];
    float x1, y1, z1; //牙龈分割平面法向量
    x1 = mGingivaCuttingPlaneNormal[0];
    y1 = mGingivaCuttingPlaneNormal[1];
    z1 = mGingivaCuttingPlaneNormal[2];
    for(vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        tempVertex = mToothMesh.point(*vertexIter);
        if(x1 * (tempVertex[0] - x0) + y1 * (tempVertex[1] - y0) + z1 * (tempVertex[2] - z0) > 0)
        {
            break;
        }
    }

    //牙龈区域生长
    regionGrowing(*vertexIter, GINGIVA_REGION);

    //去除噪声区域+分别标记牙齿区域
    mToothNum = 0; //牙齿标号（数量）
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) || mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexIter))
        {
            continue;
        }
        int regionVertexNum = regionGrowing(*vertexIter, TEMP_REGION);
        //测试，输出该区域顶点数量
        cout << "区域顶点数量: " << regionVertexNum << endl;
        if(regionVertexNum < mToothMesh.mVertexNum * 0.001) //TODO 这个阈值是臆想的，但是达到了效果
        {
            regionGrowing(*vertexIter, FILL_BOUNDARY_REGION); //如果区域小于某个阈值，则将其填充为边界
        }
        else
        {
            regionGrowing(*vertexIter, TOOTH_REGION + mToothNum);
            mToothNum++;
        }
    }
}

int ToothSegmentation::regionGrowing(Mesh::VertexHandle seedVertexHandle, int regionType)
{
    mToothMesh.property(mVPropHandleRegionGrowingVisited, seedVertexHandle) = true;
    if(regionType == FILL_BOUNDARY_REGION) //如果regionType为FILL_BOUNDARY_REGION，则将此区域填充为边界
    {
        mToothMesh.property(mVPropHandleIsToothBoundary, seedVertexHandle) = true;
    }
    else
    {
        mToothMesh.property(mVPropHandleNonBoundaryRegionType, seedVertexHandle) = regionType;
    }

    int regionVertexNum = 1; //该区域中顶点数量
    list<Mesh::VertexHandle> seeds; //种子点列表
    seeds.push_back(seedVertexHandle);
    while(!seeds.empty())
    {
        Mesh::VertexHandle vertexHandle = seeds.front();
        seeds.pop_front();
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(vertexHandle); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
            {
                continue;
            }
            if(mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexVertexIter))
            {
                continue;
            }
            mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexVertexIter) = true;
            if(regionType == FILL_BOUNDARY_REGION) //如果regionType为FILL_BOUNDARY_REGION，则将此区域填充为边界
            {
                mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter) = true;
            }
            else
            {
                mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexIter) = regionType;
            }
            seeds.push_back(*vertexVertexIter);
            regionVertexNum++;
        }
    }

    //如果regionType为TEMP_REGION，则还原该区域所有点的visited为false
    if(regionType == TEMP_REGION)
    {
        seeds.clear();
        seeds.push_back(seedVertexHandle);
        while(!seeds.empty())
        {
            Mesh::VertexHandle vertexHandle = seeds.front();
            seeds.pop_front();
            for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(vertexHandle); vertexVertexIter.is_valid(); vertexVertexIter++)
            {
                if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
                {
                    continue;
                }
                if(!mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexVertexIter))
                {
                    continue;
                }
                mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexVertexIter) = false;
                seeds.push_back(*vertexVertexIter);
            }
        }
    }

    return regionVertexNum;
}

void ToothSegmentation::paintClassifiedNonBoundaryRegions(QProgressDialog &progress)
{
    int vertexIndex = 0;
    int regionType;
    Mesh::Color colorBlue(0.0, 0.0, 1.0), colorGreen(0.0, 1.0, 0.0), colorWhite(1.0, 1.0, 1.0);
    progress.setLabelText("Painting classified nonboundary regions...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter))
        {
            //mToothMesh.set_color(*vertexIter, colorGreen);
        }
        else
        {
            regionType = mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexIter);
            switch(regionType)
            {
            case TEMP_REGION: //正常情况不会有TEMP_REGION
                mToothMesh.set_color(*vertexIter, colorWhite);
                break;
            case GINGIVA_REGION:
                mToothMesh.set_color(*vertexIter, colorBlue);
                break;
            default:
                float grayValue;
                Mesh::Color pseudoColor;
                int toothIndex = regionType - TOOTH_REGION;
                grayValue = (float)(toothIndex + 1) / (mToothNum + 1); //toothIndex+1是因为灰度为0时得到的伪彩色是蓝色，和牙龈的颜色一样，所以跳过这个颜色；mToothNum+1是因为灰度为1时得到的伪彩色是红色，和边界的颜色一样，所以跳过这个颜色
                gray2PseudoColor(grayValue, pseudoColor);
                mToothMesh.set_color(*vertexIter, pseudoColor);
                break;
            }
        }
        vertexIndex++;
    }
}

void ToothSegmentation::saveToothMesh(string filename)
{
    OpenMesh::IO::Options options;
    options += OpenMesh::IO::Options::VertexColor;
    options += OpenMesh::IO::Options::ColorFloat;
    if(!OpenMesh::IO::write_mesh(mToothMesh, filename, options))
    {
        cerr << "Failed to save tooth mesh to file: " + filename << endl;
    }
}

void ToothSegmentation::saveExtraMesh(string filename)
{
    OpenMesh::IO::Options options;
    options += OpenMesh::IO::Options::VertexColor;
    options += OpenMesh::IO::Options::ColorFloat;
    if(!OpenMesh::IO::write_mesh(mExtraMesh, filename, options))
    {
        cerr << "Failed to save extra mesh to file: " + filename << endl;
    }
}

void ToothSegmentation::gray2PseudoColor(float grayValue, Mesh::Color &pseudoColor)
{
    //将grayValue规范化到0～1之间
    if(grayValue < 0.0)
    {
        grayValue = 0.0;
    }
    if(grayValue > 1.0)
    {
        grayValue = 1.0;
    }

    if(grayValue < 0.25)
    {
        pseudoColor[0] = 0.0;
        pseudoColor[1] = grayValue * 4.0;
        pseudoColor[2] = 1.0;
    }
    else if(grayValue < 0.5)
    {
        pseudoColor[0] = 0.0;
        pseudoColor[1] = 1.0;
        pseudoColor[2] = 1.0 - (grayValue - 0.25) * 4.0;
    }
    else if(grayValue < 0.75)
    {
        pseudoColor[0] = (grayValue - 0.5) * 4.0;
        pseudoColor[1] = 1.0;
        pseudoColor[2] = 0.0;
    }
    else
    {
        pseudoColor[0] = 1.0;
        pseudoColor[1] = 1.0 - (grayValue - 0.75) * 4.0;
        pseudoColor[2] = 0.0;
    }
}

void ToothSegmentation::refineToothBoundary(bool loadStateFromFile)
{
    QProgressDialog progress(mParentWidget);
    progress.setWindowTitle("Refine tooth boundary...");
    progress.setMinimumSize(400, 120);
    progress.setCancelButtonText("cancel");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    progress.setAutoClose(false);

    refineToothBoundary(progress);

    paintAllVerticesWhite(progress);
    paintClassifiedBoundary(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.off");
    paintClassifiedNonBoundaryRegions(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithRegionGrowing.off");

    //关闭进度条
    progress.close();
}

void ToothSegmentation::refineToothBoundary(QProgressDialog &progress)
{
    /*//分别处理每个牙齿的轮廓（改成了对由cutting point分成的每段轮廓分别进行插值）
    for(int toothIndex = 0; toothIndex < mToothNum; toothIndex++)
    {
        QVector<Mesh::VertexHandle> toothBoundaryContour; //轮廓点集
        bool found; //是否已找到一个挨着该牙齿的边界点

        //寻找该牙齿轮廓上的第1个点
        found = false;
        for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
        {
            if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非边界点
            {
                continue;
            }
            for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(*vertexIter); vertexVertexIter.is_valid(); vertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
            {
                if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
                {
                    continue;
                }
                if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexIter) == (TOOTH_REGION + toothIndex))
                {
                    found = true;
                    break;
                }
            }
            if(found)
            {
                toothBoundaryContour.push_back(*vertexIter);

                //测试，输出找到的轮廓点坐标并涂红
                cout << "轮廓点：" << mToothMesh.point(*vertexIter) << endl;
                mToothMesh.set_color(*vertexIter, colorRed);

                break;
            }
        }

        //寻找第2个轮廓点（第1个轮廓点邻域中挨着该牙齿的边界点）
        found = false;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(toothBoundaryContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过非边界点
            {
                continue;
            }
            for(Mesh::VertexVertexIter vertexVertexVertexIter = mToothMesh.vv_iter(*vertexVertexIter); vertexVertexVertexIter.is_valid(); vertexVertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
            {
                if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexVertexIter))
                {
                    continue;
                }
                if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexVertexIter) == (TOOTH_REGION + toothIndex))
                {
                    found = true;
                    break;
                }
            }
            if(found)
            {
                toothBoundaryContour.push_back(*vertexVertexIter);

                //测试，输出找到的轮廓点坐标
                cout << "轮廓点：" << mToothMesh.point(*vertexVertexIter) << endl;
                mToothMesh.set_color(*vertexVertexIter, colorRed);

                break;
            }
        }

        //寻找第3个轮廓点（第2个轮廓点邻域中除第1个轮廓点之外的挨着该牙齿的边界点）
        Mesh::Point firstContourVertex = mToothMesh.point(toothBoundaryContour.front()); //第1个轮廓点
        found = false;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(toothBoundaryContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过非边界点
            {
                continue;
            }
            for(Mesh::VertexVertexIter vertexVertexVertexIter = mToothMesh.vv_iter(*vertexVertexIter); vertexVertexVertexIter.is_valid(); vertexVertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
            {
                if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexVertexIter))
                {
                    continue;
                }
                if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexVertexIter) == (TOOTH_REGION + toothIndex))
                {
                    found = true;
                    break;
                }
            }
            if(found && mToothMesh.point(*vertexVertexIter) != firstContourVertex) //找到并且不是第1个轮廓点
            {
                toothBoundaryContour.push_back(*vertexVertexIter);

                //测试，输出找到的轮廓点坐标
                cout << "轮廓点：" << mToothMesh.point(*vertexVertexIter) << endl;
                mToothMesh.set_color(*vertexVertexIter, colorRed);

                break;
            }
        }

        //深度搜索获取轮廓点集（现在已找到3个轮廓点）
        Mesh::Point previousContourVertex; //目前找到的倒数第2个轮廓点
        Mesh::Point tempContourVertex; //刚刚找到但未确定的轮廓点
        bool contourFinished = false; //该牙齿的轮廓搜索完毕
        while(true)
        {
            found = false;
            previousContourVertex = mToothMesh.point(toothBoundaryContour.at(toothBoundaryContour.size() - 2));
            for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(toothBoundaryContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
            {
                if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过非边界点
                {
                    continue;
                }
                for(Mesh::VertexVertexIter vertexVertexVertexIter = mToothMesh.vv_iter(*vertexVertexIter); vertexVertexVertexIter.is_valid(); vertexVertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
                {
                    if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexVertexIter))
                    {
                        continue;
                    }
                    if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexVertexIter) == (TOOTH_REGION + toothIndex))
                    {
                        found = true;
                        break;
                    }
                }
                if(found)
                {
                    tempContourVertex = mToothMesh.point(*vertexVertexIter);
                    if(tempContourVertex == previousContourVertex)
                    {
                        continue;
                    }
                    else if(tempContourVertex == firstContourVertex) //该牙齿的轮廓搜索完毕
                    {
                        contourFinished = true;
                        break;
                    }
                    else
                    {
                        toothBoundaryContour.push_back(*vertexVertexIter);

                        //测试，输出找到的轮廓点坐标
                        cout << "轮廓点：" << mToothMesh.point(*vertexVertexIter) << endl;
                        mToothMesh.set_color(*vertexVertexIter, colorRed);

                        break;
                    }
                }
            }
            if(contourFinished)
            {
                break;
            }
        }

        *//*//计算投影平面
        //计算质心
        Mesh::Point centerPoint(0.0, 0.0, 0.0);
        int contourVertexNum = toothBoundaryContour.size();
        int contourVertexIndex;
        for(contourVertexIndex = 0; contourVertexIndex < contourVertexNum; contourVertexIndex++)
        {
            centerPoint += mToothMesh.point(toothBoundaryContour.at(contourVertexIndex));
        }
        centerPoint /= contourVertexNum;

        //测试，输出质心点
        cout << "单个牙齿轮廓质心点：\n" << centerPoint << endl;

        //计算协方差矩阵
        Matrix3f covarMat;
        covarMat.setZero(3, 3);
        Matrix3f tempMat;
        Mesh::Point tempVertex;
        for(contourVertexIndex = 0; contourVertexIndex < contourVertexNum; contourVertexIndex++)
        {
            tempVertex = mToothMesh.point(toothBoundaryContour.at(contourVertexIndex));
            tempVertex -= centerPoint;
            tempMat << tempVertex[0] * tempVertex[0], tempVertex[0] * tempVertex[1], tempVertex[0] * tempVertex[2],
                    tempVertex[1] * tempVertex[0], tempVertex[1] * tempVertex[1], tempVertex[1] * tempVertex[2],
                    tempVertex[2] * tempVertex[0], tempVertex[2] * tempVertex[1], tempVertex[2] * tempVertex[2];
            covarMat += tempMat;
        }
        covarMat /= contourVertexNum;

        //测试，输出协方差矩阵
        cout << "单个牙齿轮廓协方差矩阵：\n" << covarMat << endl;

        //计算分割平面法向量（即协方差矩阵对应最小特征值的特征向量）
        EigenSolver<Matrix3f> eigenSolver(covarMat, true);
        EigenSolver<Matrix3f>::EigenvalueType eigenvalues = eigenSolver.eigenvalues();
        EigenSolver<Matrix3f>::EigenvectorsType eigenvectors = eigenSolver.eigenvectors();
        float eigenvalueMin = eigenvalues(0).real();
        int eigenvalueMinIndex = 0;
        for(int i = 1; i < 3; i++)
        {
            if(eigenvalues(i).real() < eigenvalueMin)
            {
                eigenvalueMin = eigenvalues(i).real();
                eigenvalueMinIndex = i;
            }
        }
        Mesh::Normal planeNormal = -Mesh::Normal(eigenvectors(0, eigenvalueMinIndex).real(), eigenvectors(1, eigenvalueMinIndex).real(), eigenvectors(2, eigenvalueMinIndex).real());

        //测试，输出法向量
        cout << "单个牙齿轮廓法向量：\n" << planeNormal << endl;

        //创建投影平面
        float contourBoundingBoxMaxLength = 0;
        tempVertex = mToothMesh.point(toothBoundaryContour.front());
        float minX = tempVertex[0], maxX = tempVertex[0];
        float minY = tempVertex[1], maxY = tempVertex[1];
        float minZ = tempVertex[2], maxZ = tempVertex[2];
        for(contourVertexIndex = 0; contourVertexIndex < contourVertexNum; contourVertexIndex++)
        {
            tempVertex = mToothMesh.point(toothBoundaryContour.at(contourVertexIndex));
            if(tempVertex[0] < minX)
            {
                minX = tempVertex[0];
            }
            else if(tempVertex[0] > maxX)
            {
                maxX = tempVertex[0];
            }
            if(tempVertex[1] < minY)
            {
                minY = tempVertex[1];
            }
            else if(tempVertex[1] > maxY)
            {
                maxY = tempVertex[1];
            }
            if(tempVertex[2] < minZ)
            {
                minZ = tempVertex[2];
            }
            else if(tempVertex[2] > maxZ)
            {
                maxZ = tempVertex[2];
            }
        }
        contourBoundingBoxMaxLength = maxX - minX;
        if(maxY - minY > contourBoundingBoxMaxLength)
        {
            contourBoundingBoxMaxLength = maxY - minY;
        }
        if(maxZ - minZ > contourBoundingBoxMaxLength)
        {
            contourBoundingBoxMaxLength = maxZ - minZ;
        }
        float ContourProjectPlaneSize = contourBoundingBoxMaxLength * 5;
        createPlaneInExtraMesh(centerPoint, planeNormal, ContourProjectPlaneSize);

        //测试，保存该牙齿的轮廓和投影平面mesh
        stringstream ss;
        ss << toothIndex;
        string toothIndexString = ss.str();
        saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Tooth" + toothIndexString + "Contour.off");
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.Tooth" + toothIndexString + "ContourProjectPlane.off");*//*

        //测试，保存显示该牙齿轮廓的牙齿模型
        stringstream ss;
        ss << toothIndex;
        string toothIndexString = ss.str();
        saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithTooth" + toothIndexString + "Contour.off");

        //测试，将轮廓点单独保存到文件
        mExtraMesh = Mesh();
        if(!mExtraMesh.has_vertex_colors())
        {
            mExtraMesh.request_vertex_colors();
        }
        Mesh::VertexHandle tempVertexHandle;
        for(int contourVertexIndex = 0; contourVertexIndex < toothBoundaryContour.size(); contourVertexIndex++)
        {
            tempVertexHandle = mExtraMesh.add_vertex(mToothMesh.point(toothBoundaryContour.at(contourVertexIndex)));
            mExtraMesh.set_color(tempVertexHandle, colorRed);
        }
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.Tooth" + toothIndexString + "Contour.off");

        //选取插值控制点
        QVector<Mesh::VertexHandle> contourControlVertices; //控制点集
        int contourVertexNum = toothBoundaryContour.size();
        for(int contourVertexIndex = 0; contourVertexIndex < contourVertexNum; contourVertexIndex++)
        {
            if(contourVertexIndex % 10 == 0) //TODO 暂时按照等距离选取控制点
            {
                contourControlVertices.push_back(toothBoundaryContour.at(contourVertexIndex));
            }
        }

        //将插值控制点从Mesh::Point格式转换为GSL库插值函数需要的格式
        int contourControlVertexNum = contourControlVertices.size();
        contourControlVertexNum += 1; //+1是为了在序列最后添加序列的第1个值，保证插值结果是首尾相连并且平滑
        double *t = new double[contourControlVertexNum];
        double *x = new double[contourControlVertexNum];
        double *y = new double[contourControlVertexNum];
        double *z = new double[contourControlVertexNum];
        Mesh::Point tempContourControlVertex;
        for(int contourControlVertexIndex = 0; contourControlVertexIndex < contourControlVertexNum - 1; contourControlVertexIndex++)
        {
            t[contourControlVertexIndex] = contourControlVertexIndex;
            tempContourControlVertex = mToothMesh.point(contourControlVertices.at(contourControlVertexIndex));
            x[contourControlVertexIndex] = tempContourControlVertex[0];
            y[contourControlVertexIndex] = tempContourControlVertex[1];
            z[contourControlVertexIndex] = tempContourControlVertex[2];
        }
        t[contourControlVertexNum - 1] = contourControlVertexNum - 1;
        tempContourControlVertex = mToothMesh.point(contourControlVertices.front());
        x[contourControlVertexNum - 1] = tempContourControlVertex[0];
        y[contourControlVertexNum - 1] = tempContourControlVertex[1];
        z[contourControlVertexNum - 1] = tempContourControlVertex[2];

        //开始插值
        gsl_interp_accel *acc = gsl_interp_accel_alloc();
        gsl_spline *spline = gsl_spline_alloc(gsl_interp_cspline, contourControlVertexNum);
        int contourInterpPointNum = (contourControlVertexNum - 1) * 100; //TODO 此处插值密度有待自动化计算
        int contourInterpPointIndex;
        double *tInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tInterp[contourInterpPointIndex] = contourInterpPointIndex * 0.01;
        }
        gsl_spline_init(spline, t, x, contourControlVertexNum);
        double *xInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            xInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }
        gsl_spline_init(spline, t, y, contourControlVertexNum);
        double *yInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            yInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }
        gsl_spline_init(spline, t, z, contourControlVertexNum);
        double *zInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            zInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }

        gsl_spline_free(spline);
        gsl_interp_accel_free(acc);
        delete[] t;
        delete[] x;
        delete[] y;
        delete[] z;
        delete[] tInterp;

        //将插值得到的数据转换回Mesh::Point格式
        QVector<Mesh::Point> contourInterpPoints; //插值得到的轮廓点集
        Mesh::Point tempContourInterpVertex;
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tempContourInterpVertex[0] = xInterp[contourInterpPointIndex];
            tempContourInterpVertex[1] = yInterp[contourInterpPointIndex];
            tempContourInterpVertex[2] = zInterp[contourInterpPointIndex];
            contourInterpPoints.push_back(tempContourInterpVertex);
        }

        delete[] xInterp;
        delete[] yInterp;
        delete[] zInterp;

        //测试，将插值得到的轮廓点保存到文件
        mExtraMesh = Mesh();
        if(!mExtraMesh.has_vertex_colors())
        {
            mExtraMesh.request_vertex_colors();
        }
        Mesh::Color colorBlue(0.0, 0.0, 1.0);
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tempVertexHandle = mExtraMesh.add_vertex(contourInterpPoints.at(contourInterpPointIndex));
            mExtraMesh.set_color(tempVertexHandle, colorBlue);
        }
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.Tooth" + toothIndexString + "ContourInterp.off");

        *//*////根据插值结果在mesh上重新搜寻轮廓点
        QVector<Mesh::VertexHandle> interpContour; //插值后映射回mesh的轮廓点集

        //第1个轮廓点
        interpContour.push_back(contourControlVertices.front());

        //第2个轮廓点
        Mesh::VertexHandle nearestVertexHandle; //距离插值曲线最近的顶点handle
        float distanceToInterpCurve, minDistanceToInterpCurve; //邻域定点到插值曲线的距离和最小距离
        float tempDistanceToInterpPoint;
        Mesh::Point tempInterpContourVertex, tempInterpPoint;
        minDistanceToInterpCurve = 1e10;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(interpContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            tempInterpContourVertex = mToothMesh.point(*vertexVertexIter);
            distanceToInterpCurve = 1e10; //TODO 设置一个足够大的数，或者设置为其与第1个插值点的距离
            //计算每个邻域顶点到插值曲线的距离
            for(contourInterpVertexIndex = 0; contourInterpVertexIndex < contourInterpVertexNum; contourInterpVertexIndex++)
            {
                tempInterpPoint = contourInterpPoints.at(contourInterpVertexIndex);
                tempDistanceToInterpPoint = (tempInterpContourVertex[0] - tempInterpPoint[0]) * (tempInterpContourVertex[0] - tempInterpPoint[0]) + (tempInterpContourVertex[1] - tempInterpPoint[1]) * (tempInterpContourVertex[1] - tempInterpPoint[1]) + (tempInterpContourVertex[2] - tempInterpPoint[2]) * (tempInterpContourVertex[2] - tempInterpPoint[2]);
                if(tempDistanceToInterpPoint < distanceToInterpCurve)
                {
                    distanceToInterpCurve = tempDistanceToInterpPoint;
                }
            }
            if(distanceToInterpCurve < minDistanceToInterpCurve)
            {
                minDistanceToInterpCurve = distanceToInterpCurve;
                nearestVertexHandle = *vertexVertexIter;
            }
        }
        interpContour.push_back(nearestVertexHandle);

        //第3个轮廓点
        minDistanceToInterpCurve = 1e10;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(interpContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            tempInterpContourVertex = mToothMesh.point(*vertexVertexIter);
            if(tempInterpContourVertex == mToothMesh.point(interpContour.front())) //跳过第1个轮廓点
            {
                continue;
            }
            if(cos(tempInterpContourVertex - mToothMesh.point(interpContour.back()), mToothMesh.point(interpContour.at(interpContour.size() - 2)) - mToothMesh.point(interpContour.back())) > -0.5) //限制只能往前搜索而不能后退
            {
                continue;
            }
            distanceToInterpCurve = 1e10;
            //计算每个邻域顶点到插值曲线的距离
            for(contourInterpVertexIndex = 0; contourInterpVertexIndex < contourInterpVertexNum; contourInterpVertexIndex++)
            {
                tempInterpPoint = contourInterpPoints.at(contourInterpVertexIndex);
                tempDistanceToInterpPoint = (tempInterpContourVertex[0] - tempInterpPoint[0]) * (tempInterpContourVertex[0] - tempInterpPoint[0]) + (tempInterpContourVertex[1] - tempInterpPoint[1]) * (tempInterpContourVertex[1] - tempInterpPoint[1]) + (tempInterpContourVertex[2] - tempInterpPoint[2]) * (tempInterpContourVertex[2] - tempInterpPoint[2]);
                if(tempDistanceToInterpPoint < distanceToInterpCurve)
                {
                    distanceToInterpCurve = tempDistanceToInterpPoint;
                }
            }
            if(distanceToInterpCurve < minDistanceToInterpCurve)
            {
                minDistanceToInterpCurve = distanceToInterpCurve;
                nearestVertexHandle = *vertexVertexIter;
            }
        }
        interpContour.push_back(nearestVertexHandle);

        //深度搜索获取轮廓点集（现在已找到3个轮廓点）
        contourFinished = false;
        bool duplicated;
        while(true)
        {
            minDistanceToInterpCurve = 1e10;
            for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(interpContour.back()); vertexVertexIter.is_valid(); vertexVertexIter++)
            {
                tempInterpContourVertex = mToothMesh.point(*vertexVertexIter);
                if(tempInterpContourVertex == mToothMesh.point(interpContour.at(interpContour.size() - 2))) //跳过前1个轮廓点
                {
                    continue;
                }
                if(tempInterpContourVertex == mToothMesh.point(interpContour.front())) //如果邻域中找到了第1个轮廓点，说明轮廓已搜寻完毕
                {
                    contourFinished = true;
                    break;
                }
                if(cos(tempInterpContourVertex - mToothMesh.point(interpContour.back()), mToothMesh.point(interpContour.at(interpContour.size() - 2)) - mToothMesh.point(interpContour.back())) > -0.5) //限制只能往前搜索而不能后退
                {
                    continue;
                }

                //不能添加重复的顶点
                duplicated = false;
                for(int interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
                {
                    if(tempInterpContourVertex == mToothMesh.point(interpContour.at(interpContourVertexIndex)))
                    {
                        duplicated = true;
                        break;
                    }
                }
                if(duplicated)
                {
                    continue;
                }

                distanceToInterpCurve = 1e10;
                //计算每个邻域顶点到插值曲线的距离
                for(contourInterpVertexIndex = 0; contourInterpVertexIndex < contourInterpVertexNum; contourInterpVertexIndex++)
                {
                    tempInterpPoint = contourInterpPoints.at(contourInterpVertexIndex);
                    tempDistanceToInterpPoint = (tempInterpContourVertex[0] - tempInterpPoint[0]) * (tempInterpContourVertex[0] - tempInterpPoint[0]) + (tempInterpContourVertex[1] - tempInterpPoint[1]) * (tempInterpContourVertex[1] - tempInterpPoint[1]) + (tempInterpContourVertex[2] - tempInterpPoint[2]) * (tempInterpContourVertex[2] - tempInterpPoint[2]);
                    if(tempDistanceToInterpPoint < distanceToInterpCurve)
                    {
                        distanceToInterpCurve = tempDistanceToInterpPoint;
                    }
                }
                if(distanceToInterpCurve < minDistanceToInterpCurve)
                {
                    minDistanceToInterpCurve = distanceToInterpCurve;
                    nearestVertexHandle = *vertexVertexIter;
                }
            }
            if(contourFinished)
            {
                break;
            }
            else
            {
                interpContour.push_back(nearestVertexHandle);
            }

            //测试
            if(interpContour.size() > 200)
                break;
        }*//*

        //利用K近邻搜索获取mesh上距离插值轮廓最近的顶点集合
        QVector<Mesh::Point> toothMeshVertices;
        QVector<Mesh::VertexHandle> toothMeshVertexHandles;
        for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
        {
            toothMeshVertices.push_back(mToothMesh.point(*vertexIter));
            toothMeshVertexHandles.push_back(*vertexIter);
        }
        int knn = 4; //对于插值轮廓上的每一个点，在mesh上寻找前k个与其最近的顶点
        QVector< QVector<int> > searchResult = kNearestNeighbours(progress, knn, contourInterpPoints, toothMeshVertices);
        QVector<Mesh::VertexHandle> interpContour; //插值后映射回mesh的轮廓点集
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            for(int k = 0; k < knn; k++)
            {
                tempVertexHandle = toothMeshVertexHandles.at(searchResult[contourInterpPointIndex][k]);
                if(!interpContour.contains(tempVertexHandle))
                {
                    interpContour.push_back(tempVertexHandle);
                }
            }
        }

        //测试，将插值后的轮廓点单独保存到文件
        mExtraMesh = Mesh();
        if(!mExtraMesh.has_vertex_colors())
        {
            mExtraMesh.request_vertex_colors();
        }
        Mesh::Color colorYellow(1.0, 1.0, 0.0);
        int interpContourVertexIndex;
        for(interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
        {
            tempVertexHandle = mExtraMesh.add_vertex(mToothMesh.point(interpContour.at(interpContourVertexIndex)));
            mExtraMesh.set_color(tempVertexHandle, colorYellow);
        }
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.Tooth" + toothIndexString + "InterpContour.off");

        //测试，保存显示该牙齿轮廓（插值后）的牙齿模型
        paintAllVerticesWhite(progress);
        for(interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
        {
            mToothMesh.set_color(interpContour.at(interpContourVertexIndex), colorRed);
        }
        saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithTooth" + toothIndexString + "InterpContour.off");

        //测试
        break;

    }*/

    //寻找所有轮廓曲线段（两个cutting point之间的轮廓线）
    indexContourSectionsVertices(progress);

    Mesh::Color colorRed(1.0, 0.0, 0.0);

    //分别处理每一个contour section，选取控制点、插值、找近邻区域、细化
    int contourSectionIndex;
    progress.setLabelText("Interpolating all contour sections...");
    progress.setMinimum(0);
    progress.setMaximum(mContourSections.size());
    progress.setValue(0);
    for(contourSectionIndex = 0; contourSectionIndex < mContourSections.size(); contourSectionIndex++)
    {
        progress.setValue(contourSectionIndex);

        //选取插值控制点
        QVector<Mesh::VertexHandle> contourControlVertices; //控制点集
        int contourVertexNum = mContourSections[contourSectionIndex].size();
        if(contourVertexNum < 21) //如果轮廓太短，则不进行处理
        {
            continue;
        }
        for(int contourVertexIndex = 0; contourVertexIndex < contourVertexNum; contourVertexIndex++)
        {
            if(contourVertexIndex % 10 == 0 || contourVertexIndex == contourVertexNum - 1) //TODO 暂时按照等距离选取控制点
            {
                contourControlVertices.push_back(mContourSections[contourSectionIndex].at(contourVertexIndex));
            }
        }

        //将插值控制点从Mesh::Point格式转换为GSL库插值函数需要的格式
        int contourControlVertexNum = contourControlVertices.size();
        double *t = new double[contourControlVertexNum];
        double *x = new double[contourControlVertexNum];
        double *y = new double[contourControlVertexNum];
        double *z = new double[contourControlVertexNum];
        Mesh::Point tempContourControlVertex;
        for(int contourControlVertexIndex = 0; contourControlVertexIndex < contourControlVertexNum; contourControlVertexIndex++)
        {
            t[contourControlVertexIndex] = contourControlVertexIndex;
            tempContourControlVertex = mToothMesh.point(contourControlVertices.at(contourControlVertexIndex));
            x[contourControlVertexIndex] = tempContourControlVertex[0];
            y[contourControlVertexIndex] = tempContourControlVertex[1];
            z[contourControlVertexIndex] = tempContourControlVertex[2];
        }

        //开始插值
        gsl_interp_accel *acc = gsl_interp_accel_alloc();
        gsl_spline *spline = gsl_spline_alloc(gsl_interp_cspline, contourControlVertexNum);
        int contourInterpPointNum = (contourControlVertexNum - 1) * 100; //TODO 此处插值密度有待自动化计算
        int contourInterpPointIndex;
        double *tInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tInterp[contourInterpPointIndex] = contourInterpPointIndex * 0.01;
        }
        gsl_spline_init(spline, t, x, contourControlVertexNum);
        double *xInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            xInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }
        gsl_spline_init(spline, t, y, contourControlVertexNum);
        double *yInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            yInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }
        gsl_spline_init(spline, t, z, contourControlVertexNum);
        double *zInterp = new double[contourInterpPointNum];
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            zInterp[contourInterpPointIndex] = gsl_spline_eval(spline, tInterp[contourInterpPointIndex], acc);
        }

        gsl_spline_free(spline);
        gsl_interp_accel_free(acc);
        delete[] t;
        delete[] x;
        delete[] y;
        delete[] z;
        delete[] tInterp;

        //将插值得到的数据转换回Mesh::Point格式
        QVector<Mesh::Point> contourInterpPoints; //插值得到的轮廓点集
        Mesh::Point tempContourInterpVertex;
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tempContourInterpVertex[0] = xInterp[contourInterpPointIndex];
            tempContourInterpVertex[1] = yInterp[contourInterpPointIndex];
            tempContourInterpVertex[2] = zInterp[contourInterpPointIndex];
            contourInterpPoints.push_back(tempContourInterpVertex);
        }

        delete[] xInterp;
        delete[] yInterp;
        delete[] zInterp;

        Mesh::VertexHandle tempVertexHandle;

        //测试，将插值得到的轮廓点保存到文件
        /*mExtraMesh = Mesh();
        if(!mExtraMesh.has_vertex_colors())
        {
            mExtraMesh.request_vertex_colors();
        }
        Mesh::Color colorBlue(0.0, 0.0, 1.0);
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            tempVertexHandle = mExtraMesh.add_vertex(contourInterpPoints.at(contourInterpPointIndex));
            mExtraMesh.set_color(tempVertexHandle, colorBlue);
        }
        stringstream ss;
        ss << contourSectionIndex;
        string contourSectionIndexString = ss.str();
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.ContourSection" + contourSectionIndexString + "Interp.off");*/

        //利用K近邻搜索获取mesh上距离插值轮廓最近的顶点集合
        QVector<Mesh::Point> toothMeshVertices;
        QVector<Mesh::VertexHandle> toothMeshVertexHandles;
        for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
        {
            toothMeshVertices.push_back(mToothMesh.point(*vertexIter));
            toothMeshVertexHandles.push_back(*vertexIter);
        }
        int knn = 2; //对于插值轮廓上的每一个点，在mesh上寻找前k个与其最近的顶点
        QVector< QVector<int> > searchResult = kNearestNeighbours(progress, knn, contourInterpPoints, toothMeshVertices);
        QVector<Mesh::VertexHandle> interpContour; //插值后映射回mesh的轮廓点集
        for(contourInterpPointIndex = 0; contourInterpPointIndex < contourInterpPointNum; contourInterpPointIndex++)
        {
            for(int k = 0; k < knn; k++)
            {
                tempVertexHandle = toothMeshVertexHandles.at(searchResult[contourInterpPointIndex][k]);
                if(!interpContour.contains(tempVertexHandle))
                {
                    interpContour.push_back(tempVertexHandle);
                }
            }
        }

        //将所有近邻顶点设置为边界点
        int interpContourVertexIndex;
        for(interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
        {
            mToothMesh.property(mVPropHandleIsToothBoundary, interpContour.at(interpContourVertexIndex)) = true;
        }

        //测试，将插值后的轮廓点单独保存到文件
        /*mExtraMesh = Mesh();
        if(!mExtraMesh.has_vertex_colors())
        {
            mExtraMesh.request_vertex_colors();
        }
        Mesh::Color colorYellow(1.0, 1.0, 0.0);
        for(interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
        {
            tempVertexHandle = mExtraMesh.add_vertex(mToothMesh.point(interpContour.at(interpContourVertexIndex)));
            mExtraMesh.set_color(tempVertexHandle, colorYellow);
        }
        saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.ContourSection" + contourSectionIndexString + "InterpNearestRegion.off");*/

        //测试，保存显示该轮廓（插值后）的牙齿模型
        /*paintAllVerticesWhite(progress);
        for(interpContourVertexIndex = 0; interpContourVertexIndex < interpContour.size(); interpContourVertexIndex++)
        {
            mToothMesh.set_color(interpContour.at(interpContourVertexIndex), colorRed);
        }
        saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithContourSection" + contourSectionIndexString + "InterpNearestRegion.off");*/
    }

    //测试，保存带平滑轮廓（非单点宽度）的牙齿模型到文件
    paintAllVerticesWhite(progress);
    paintBoundaryVertices(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithInterpNearestRegion.off");

    //重新进行区域生长
    markNonBoundaryRegion(progress);

    //测试，保存带平滑轮廓（非单点宽度）并进行区域生长标记后的牙齿模型到文件
    paintClassifiedNonBoundaryRegions(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithInterpNearestRegionAndRegionGrowing.off");

    //重新进行单点宽度边界提取
    boundarySkeletonExtraction(progress);

    //测试，保存重新提取单点宽度边界后的牙齿模型到文件
    paintClassifiedNonBoundaryRegions(progress);
    paintBoundaryVertices(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.WithInterpNearestRegionSkeleton.off");

    //重新建立轮廓点索引
    findCuttingPoints(progress);
    indexContourSectionsVertices(progress);

    //模板平滑（此步骤通过移动轮廓点的位置，来使得轮廓变得平滑）
    Mesh::Point tempPoint;
    int halfWindowSize = 5; //平滑窗口半边长，窗口长度为其2倍加1
    int windowSize = halfWindowSize * 2 + 1;
    progress.setLabelText("Smoothing all contour sections...");
    progress.setMinimum(0);
    progress.setMaximum(mContourSections.size());
    progress.setValue(0);
    for(contourSectionIndex = 0; contourSectionIndex < mContourSections.size(); contourSectionIndex++)
    {
        progress.setValue(contourSectionIndex);
        if(mContourSections[contourSectionIndex].size() < windowSize)
        {
            continue;
        }
        for(int contourSectionVertexIndex = 0 + halfWindowSize; contourSectionVertexIndex < mContourSections[contourSectionIndex].size() - halfWindowSize; contourSectionVertexIndex++)
        {
            tempPoint[0] = 0.0; tempPoint[1] = 0.0; tempPoint[2] = 0.0;
            for(int i = contourSectionVertexIndex - halfWindowSize; i <= contourSectionVertexIndex + halfWindowSize; i++)
            {
                tempPoint += mToothMesh.point(mContourSections[contourSectionIndex].at(i));
            }
            tempPoint /= windowSize;
            mToothMesh.set_point(mContourSections[contourSectionIndex].at(contourSectionVertexIndex), tempPoint);
        }
    }
}

void ToothSegmentation::paintAllVerticesWhite(QProgressDialog &progress)
{
    int vertexIndex = 0;
    progress.setLabelText("Painting all vertices white...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);
    Mesh::Color colorWhite(1.0, 1.0, 1.0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        mToothMesh.set_color(*vertexIter, colorWhite);
        vertexIndex++;
    }
}

void ToothSegmentation::createPlaneInExtraMesh(Mesh::Point point, Mesh::Normal normal, float size)
{
    float halfDiagonalLineLength = size / 1.414; //正方形中心到4个顶点的距离
    float x0, y0, z0; //质心点（牙龈分割平面正方形中心点(记为点O)）
    x0 = point[0];
    y0 = point[1];
    z0 = point[2];
    float x1, y1, z1; //法向量
    x1 = normal[0];
    y1 = normal[1];
    z1 = normal[2];
    float x2, y2, z2; //牙龈分割平面上的另一个点(记为点N)，若平面不与x轴平行，则取该点为平面与x轴的交点，否则按y、z轴依此类推
    if(x1 != 0.0)
    {
        x2 = (y1 * y0 + z1 * z0) / x1 + x0;
        y2 = 0.0;
        z2 = 0.0;
    }
    else if(y1 != 0.0)
    {
        x2 = 0.0;
        y2 = (x1 * x0 + z1 * z0) / y1 + y0;
        z2 = 0;
    }
    else if(z1 != 0.0)
    {
        x2 = 0.0;
        y2 = 0.0;
        z2 = (y1 * y0 + x1 * x0) / z1 + z0;
    }
    float scale1 = halfDiagonalLineLength / sqrt((x2 - x0) * (x2 - x0) + y0 * y0 + z0 * z0); //向量ON乘以此倍数得到长度为halfDiagonalLineLength的向量（从点O指向正方形其中一个顶点(记为点A)）
    float x3, y3, z3; //正方形其中一个顶点(点A)
    x3 = x0 + (x2 - x0) * scale1;
    y3 = y0 + (y2 - y0) * scale1;
    z3 = z0 + (z2 - z0) * scale1;
    float x4, y4, z4; //正方形中与上述顶点相邻的顶点(记为点B)
    float x5, y5, z5; //与向量OB同向的向量(记为向量OM)（由法向量和OA作叉积而得）
    x5 = y1 * (z3 - z0) - z1 * (y3 - y0);
    y5 = z1 * (x3 - x0) - x1 * (z3 - z0);
    z5 = x1 * (y3 - y0) - y1 * (x3 - x0);
    float scale2 = halfDiagonalLineLength / sqrt(x5 * x5 + y5 * y5 + z5 * z5); //向量OM乘以此倍数得到向量OB
    x4 = x0 + x5 * scale2;
    y4 = y0 + y5 * scale2;
    z4 = z0 + z5 * scale2;
    float x6, y6, z6; //正方形中与顶点A相对的顶点(记为点C)（根据A、C两点关于点O对称而得）
    x6 = x0 * 2.0 - x3;
    y6 = y0 * 2.0 - y3;
    z6 = z0 * 2.0 - z3;
    float x7, y7, z7; //正方形中与顶点B相对的顶点(记为点D)（根据B、D两点关于点O对称而得）
    x7 = x0 * 2.0 - x4;
    y7 = y0 * 2.0 - y4;
    z7 = z0 * 2.0 - z4;

    //测试，输出分割平面正方形4个顶点坐标
    cout << "牙龈分割平面正方形顶点：\n"
         << "A: " << x3 << ", " << y3 << ", " << z3 << "\n"
         << "B: " << x4 << ", " << y4 << ", " << z4 << "\n"
         << "C: " << x6 << ", " << y6 << ", " << z6 << "\n"
         << "D: " << x7 << ", " << y7 << ", " << z7 << endl;

    //将分割平面添加到mExtraMesh中，以便显示
    mExtraMesh = Mesh();
    Mesh::VertexHandle vertexHandles[4];
    vertexHandles[0] = mExtraMesh.add_vertex(Mesh::Point(x3, y3, z3));
    vertexHandles[1] = mExtraMesh.add_vertex(Mesh::Point(x4, y4, z4));
    vertexHandles[2] = mExtraMesh.add_vertex(Mesh::Point(x6, y6, z6));
    vertexHandles[3] = mExtraMesh.add_vertex(Mesh::Point(x7, y7, z7));
    vector<Mesh::VertexHandle> faceVertexhandles;
    faceVertexhandles.clear();
    faceVertexhandles.push_back(vertexHandles[0]);
    faceVertexhandles.push_back(vertexHandles[1]);
    faceVertexhandles.push_back(vertexHandles[2]);
    faceVertexhandles.push_back(vertexHandles[3]);
    mExtraMesh.add_face(faceVertexhandles);
    if(!mExtraMesh.has_vertex_normals())
    {
        mExtraMesh.request_vertex_normals();
    }
    mExtraMesh.set_normal(vertexHandles[0], normal);
    mExtraMesh.set_normal(vertexHandles[1], normal);
    mExtraMesh.set_normal(vertexHandles[2], normal);
    mExtraMesh.set_normal(vertexHandles[3], normal);
    Mesh::FaceHandle faceHandle = *(mExtraMesh.vf_iter(vertexHandles[0]));
    if(!mExtraMesh.has_face_normals())
    {
        mExtraMesh.request_face_normals();
    }
    mExtraMesh.set_normal(faceHandle, normal);
    Mesh::Color colorGreen(0.3, 1.0, 0.0); //TODO 将分割平面颜色设置成半透明（经测试，直接在Mesh::draw()方法中将glColor3f()改为glColor4f()并添加alpha参数并不会使的模型显示透明效果）
    if(!mExtraMesh.has_vertex_colors())
    {
        mExtraMesh.request_vertex_colors();
    }
    mExtraMesh.set_color(vertexHandles[0], colorGreen);
    mExtraMesh.set_color(vertexHandles[1], colorGreen);
    mExtraMesh.set_color(vertexHandles[2], colorGreen);
    mExtraMesh.set_color(vertexHandles[3], colorGreen);
    if(!mExtraMesh.has_face_colors())
    {
        mExtraMesh.request_face_colors();
    }
    mExtraMesh.set_color(faceHandle, colorGreen);
}

void ToothSegmentation::findCuttingPoints(bool loadStateFromFile)
{
    QProgressDialog progress(mParentWidget);
    progress.setWindowTitle("Finding cutting points...");
    progress.setMinimumSize(400, 120);
    progress.setCancelButtonText("cancel");
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    progress.setAutoClose(false);

    findCuttingPoints(progress);

    paintAllVerticesWhite(progress);
    paintClassifiedBoundary(progress);
    saveToothMesh(mToothMesh.MeshName.toStdString() + ".FindCuttingPoints.off");

    //关闭进度条
    progress.close();
}

bool ToothSegmentation::saveState(QProgressDialog &progress, string stateSymbol)
{
    string stateFileName = mToothMesh.MeshName.toStdString() + "." + stateSymbol + ".State";
    QFile stateFile(stateFileName.c_str());
    if(!stateFile.open(QIODevice::WriteOnly))
    {
        cout << "Fail to open file \"" << stateFileName << "\" ." << endl;
        return false;
    }

    progress.setLabelText("Saving state...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);

    stateFile.write((char *)(&mBoundaryVertexNum), sizeof(mBoundaryVertexNum));
    stateFile.write((char *)(&mGingivaCuttingPlanePoint), sizeof(mGingivaCuttingPlanePoint));
    stateFile.write((char *)(&mGingivaCuttingPlaneNormal), sizeof(mGingivaCuttingPlaneNormal));
    stateFile.write((char *)(&mToothNum), sizeof(mToothNum));

    double tempCurvature;
    bool tempCurvatureComputed;
    bool tempIsToothBoundary;
    int tempBoundaryVertexType;
    int tempNonBoundaryRegionType;
    bool tempRegionGrowingVisited;
    int tempBoundaryType;
    Mesh::Color tempColor;
    int vertexIndex = 0;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        tempCurvature = mToothMesh.property(mVPropHandleCurvature, *vertexIter);
        stateFile.write((char *)(&tempCurvature), sizeof(tempCurvature));
        tempCurvatureComputed = mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter);
        stateFile.write((char *)(&tempCurvatureComputed), sizeof(tempCurvatureComputed));
        tempIsToothBoundary = mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter);
        stateFile.write((char *)(&tempIsToothBoundary), sizeof(tempIsToothBoundary));
        tempBoundaryVertexType = mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter);
        stateFile.write((char *)(&tempBoundaryVertexType), sizeof(tempBoundaryVertexType));
        tempNonBoundaryRegionType = mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexIter);
        stateFile.write((char *)(&tempNonBoundaryRegionType), sizeof(tempNonBoundaryRegionType));
        tempRegionGrowingVisited = mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexIter);
        stateFile.write((char *)(&tempRegionGrowingVisited), sizeof(tempRegionGrowingVisited));
        tempBoundaryType = mToothMesh.property(mVPropHandleBoundaryType, *vertexIter);
        stateFile.write((char *)(&tempBoundaryType), sizeof(tempBoundaryType));
        tempColor = mToothMesh.color(*vertexIter);
        stateFile.write((char *)(&tempColor), sizeof(tempColor));
        vertexIndex++;
    }
    stateFile.close();
    return true;
}

bool ToothSegmentation::loadState(QProgressDialog &progress, string stateSymbol)
{
    string stateFileName = mToothMesh.MeshName.toStdString() + "." + stateSymbol + ".State";
    QFile stateFile(stateFileName.c_str());
    if(!stateFile.open(QIODevice::ReadOnly))
    {
        cout << "Fail to open file \"" << stateFileName << "\" ." << endl;
        return false;
    }

    progress.setLabelText("Loading state...");
    progress.setMinimum(0);
    progress.setMaximum(mToothMesh.mVertexNum);
    progress.setValue(0);

    stateFile.read((char *)(&mBoundaryVertexNum), sizeof(mBoundaryVertexNum));
    stateFile.read((char *)(&mGingivaCuttingPlanePoint), sizeof(mGingivaCuttingPlanePoint));
    stateFile.read((char *)(&mGingivaCuttingPlaneNormal), sizeof(mGingivaCuttingPlaneNormal));
    stateFile.read((char *)(&mToothNum), sizeof(mToothNum));

    double tempCurvature;
    bool tempCurvatureComputed;
    bool tempIsToothBoundary;
    int tempBoundaryVertexType;
    int tempNonBoundaryRegionType;
    bool tempRegionGrowingVisited;
    int tempBoundaryType;
    Mesh::Color tempColor;
    int vertexIndex = 0;
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        progress.setValue(vertexIndex);
        stateFile.read((char *)(&tempCurvature), sizeof(tempCurvature));
        mToothMesh.property(mVPropHandleCurvature, *vertexIter) = tempCurvature;
        stateFile.read((char *)(&tempCurvatureComputed), sizeof(tempCurvatureComputed));
        mToothMesh.property(mVPropHandleCurvatureComputed, *vertexIter) = tempCurvatureComputed;
        stateFile.read((char *)(&tempIsToothBoundary), sizeof(tempIsToothBoundary));
        mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter) = tempIsToothBoundary;
        stateFile.read((char *)(&tempBoundaryVertexType), sizeof(tempBoundaryVertexType));
        mToothMesh.property(mVPropHandleBoundaryVertexType, *vertexIter) = tempBoundaryVertexType;
        stateFile.read((char *)(&tempNonBoundaryRegionType), sizeof(tempNonBoundaryRegionType));
        mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexIter) = tempNonBoundaryRegionType;
        stateFile.read((char *)(&tempRegionGrowingVisited), sizeof(tempRegionGrowingVisited));
        mToothMesh.property(mVPropHandleRegionGrowingVisited, *vertexIter) = tempRegionGrowingVisited;
        stateFile.read((char *)(&tempBoundaryType), sizeof(tempBoundaryType));
        mToothMesh.property(mVPropHandleBoundaryType, *vertexIter) = tempBoundaryType;
        stateFile.read((char *)(&tempColor), sizeof(tempColor));
        mToothMesh.set_color(*vertexIter, tempColor);
        vertexIndex++;
    }
    stateFile.close();
    return true;
}

QVector< QVector<int> > ToothSegmentation::kNearestNeighbours(QProgressDialog &progress, int Knn, const QVector<Mesh::Point> &querys, const QVector<Mesh::Point> &points)
{
//    progress.setLabelText("Computing k nearest neighbours...");
//    progress.setMinimum(0);
//    progress.setMaximum(querys.size());
//    progress.setValue(0);

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

    // Generate pointcloud data
    cloud->width = points.size();
    cloud->height = 1;
    cloud->points.resize(cloud->width * cloud->height);

    for(size_t i = 0; i < cloud->points.size(); i++)
    {
        cloud->points[i].x = points[i][0];
        cloud->points[i].y = points[i][1];
        cloud->points[i].z = points[i][2];
    }

    pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
    kdtree.setInputCloud(cloud);

    pcl::PointXYZ query;
    std::vector<int> pointIdxNKNSearch(Knn);
    std::vector<float> pointNKNSquaredDistance(Knn);

    QVector<QVector<int> > neighbours(querys.size());
    for(int i = 0; i < querys.size(); i++)
    {
//        progress.setValue(i);
        query.x = querys[i][0];
        query.y = querys[i][1];
        query.z = querys[i][2];
        if(kdtree.nearestKSearch(query, Knn, pointIdxNKNSearch, pointNKNSquaredDistance) > 0)
        {
            for(int j=0; j < pointIdxNKNSearch.size(); j++)
            {
                neighbours[i].append(pointIdxNKNSearch[j]);
            }
        }
    }

    return neighbours;
}

void ToothSegmentation::findCuttingPoints(QProgressDialog &progress)
{
    bool neighborHasGingivaRegion; //某顶点是否与牙龈区域相邻（若是，则可能为TOOTH_GINGIVA_BOUNDARY或CUTTING_POINT）
    int neighborBoundaryVertexNum; //某顶点邻域中边界点数量（若超过2个，并且其中没有cutting point，则为CUTTING_POINT）
    bool neighborHasCuttingPoint; //某顶点邻域中是否已有cutting point（有一种情况是cutting point处边界点构成了一个三角形，这种情况只能取其中1个作为cutting point）
    int boundaryVertexIndex;

    //初始化所有顶点的BoundaryType为除CUTTING_POINT之外的任一类型，因为在保证不能存在两个相邻的cutting point时需要知道某顶点是否属于CUTTING_POINT
    boundaryVertexIndex = 0;
    progress.setLabelText("Classifing boundary(init BoundaryType of all boundary vertices)...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非边界点
        {
            continue;
        }
        progress.setValue(boundaryVertexIndex);
        mToothMesh.property(mVPropHandleBoundaryType, *vertexIter) = TOOTH_GINGIVA_BOUNDARY;
    }

    if(!mCuttingPointHandles.empty())
    {
        mCuttingPointHandles.clear();
    }

    progress.setLabelText("Classifing boundary...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非边界点
        {
            continue;
        }

        progress.setValue(boundaryVertexIndex);

        neighborHasGingivaRegion = false;
        neighborBoundaryVertexNum = 0;
        neighborHasCuttingPoint = false;
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(*vertexIter); vertexVertexIter.is_valid(); vertexVertexIter++)
        {
            if(mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter))
            {
                neighborBoundaryVertexNum++;
                if(mToothMesh.property(mVPropHandleBoundaryType, *vertexVertexIter) == CUTTING_POINT)
                {
                    neighborHasCuttingPoint = true;
                }
                continue;
            }
            if(mToothMesh.property(mVPropHandleNonBoundaryRegionType, *vertexVertexIter) == GINGIVA_REGION)
            {
                neighborHasGingivaRegion = true;
            }
        }

        if(neighborHasGingivaRegion) //如果与牙龈区域相邻，则可能是TOOTH_GINGIVA_BOUNDARY或CUTTING_POINT
        {
            if(neighborBoundaryVertexNum > 2 && !neighborHasCuttingPoint)
            {
                mToothMesh.property(mVPropHandleBoundaryType, *vertexIter) = CUTTING_POINT;
                mCuttingPointHandles.push_back(*vertexIter);
            }
            else
            {
                mToothMesh.property(mVPropHandleBoundaryType, *vertexIter) = TOOTH_GINGIVA_BOUNDARY;
            }
        }
        else
        {
            mToothMesh.property(mVPropHandleBoundaryType, *vertexIter) = TOOTH_TOOTH_BOUNDARY;
        }

        boundaryVertexIndex++;
    }
}

void ToothSegmentation::paintClassifiedBoundary(QProgressDialog &progress)
{
    int boundaryVertexIndex = 0;
    Mesh::Color colorRed(1.0, 0.0, 0.0), colorBlue(0.0, 0.0, 1.0), colorYellow(1.0, 1.0, 0.0);
    progress.setLabelText("Painting classified boundary...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非边界点
        {
            continue;
        }

        progress.setValue(boundaryVertexIndex);

        switch(mToothMesh.property(mVPropHandleBoundaryType, *vertexIter))
        {
        case TOOTH_GINGIVA_BOUNDARY:
            mToothMesh.set_color(*vertexIter, colorRed);
            break;
        case TOOTH_TOOTH_BOUNDARY:
            mToothMesh.set_color(*vertexIter, colorBlue);
            break;
        case CUTTING_POINT:
            mToothMesh.set_color(*vertexIter, colorYellow);
            break;
        }

        boundaryVertexIndex++;
    }
}

void ToothSegmentation::indexContourSectionsVertices(QProgressDialog &progress)
{
    if(!mContourSections.empty())
    {
        mContourSections.clear();
    }

    int contourSectionIndex = 0;

    //初始化SearchContourSectionVisited属性
    int boundaryVertexIndex = 0;
    progress.setLabelText("Init SearchContourSectionVisited of all boundary vertices...");
    progress.setMinimum(0);
    progress.setMaximum(mBoundaryVertexNum);
    progress.setValue(0);
    for(Mesh::VertexIter vertexIter = mToothMesh.vertices_begin(); vertexIter != mToothMesh.vertices_end(); vertexIter++)
    {
        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexIter)) //跳过非边界点
        {
            continue;
        }
        progress.setValue(boundaryVertexIndex);
        mToothMesh.property(mVPropHandleSearchContourSectionVisited, *vertexIter) = false;
        boundaryVertexIndex++;
    }

    //从每个cutting point开始查找
    int cuttingPointIndex;
    Mesh::VertexHandle tempCuttingPointHandle;
    progress.setLabelText("Finding all contour sections...");
    progress.setMinimum(0);
    progress.setMaximum(mCuttingPointHandles.size());
    progress.setValue(0);
    for(cuttingPointIndex = 0; cuttingPointIndex < mCuttingPointHandles.size(); cuttingPointIndex++)
    {
        progress.setValue(cuttingPointIndex);
        tempCuttingPointHandle = mCuttingPointHandles.at(cuttingPointIndex);

        //查找所有以此cutting point为端点的轮廓曲线段
        for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(tempCuttingPointHandle); vertexVertexIter.is_valid(); vertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
        {
            if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过非边界点
            {
                continue;
            }
            if(!mToothMesh.property(mVPropHandleSearchContourSectionVisited, *vertexVertexIter)) //如果此邻域边界点未被访问过，则从此点开始搜索该轮廓曲线段
            {
                mToothMesh.property(mVPropHandleSearchContourSectionVisited, *vertexVertexIter) = true;
                mContourSections.push_back(QVector<Mesh::VertexHandle>());
                mContourSections[contourSectionIndex].push_back(tempCuttingPointHandle); //将该cutting point添加作为该contour section的第1个点
                mContourSections[contourSectionIndex].push_back(*vertexVertexIter); //将该邻域边界点添加作为该contour section的第2个点
                Mesh::Point previousContourVertex; //目前找到的倒数第2个轮廓点
                bool contourSectionFinished = false;
                while(true)
                {
                    previousContourVertex = mToothMesh.point(mContourSections[contourSectionIndex].at(mContourSections[contourSectionIndex].size() - 2));
                    for(Mesh::VertexVertexIter vertexVertexIter = mToothMesh.vv_iter(mContourSections[contourSectionIndex].back()); vertexVertexIter.is_valid(); vertexVertexIter++) //寻找邻域中有没有属于该牙齿区域的非边界点
                    {
                        if(!mToothMesh.property(mVPropHandleIsToothBoundary, *vertexVertexIter)) //跳过非边界点
                        {
                            continue;
                        }
                        if(mToothMesh.point(*vertexVertexIter) == previousContourVertex) //跳过之前的轮廓点
                        {
                            continue;
                        }
                        if(mToothMesh.property(mVPropHandleBoundaryType, *vertexVertexIter) == CUTTING_POINT) //到达另一个cutting point，该contour section搜索结束
                        {
                            mContourSections[contourSectionIndex].push_back(*vertexVertexIter); //将此cutting point添加作为该contour section的最后1个点
                            contourSectionFinished = true;
                            break;
                        }
                        if(mToothMesh.property(mVPropHandleSearchContourSectionVisited, *vertexVertexIter)) //不能重复添加某个点
                        {
                            continue;
                        }
                        if(mToothMesh.property(mVPropHandleBoundaryType, *vertexVertexIter) != mToothMesh.property(mVPropHandleBoundaryType, mContourSections[contourSectionIndex].back())) //某个contour section上除两个端点之外的所有点的BoundaryType应该相同（cutting point处边界连成三角形的情况如果不加此限制，会丢掉一条contour section或迭代达不到终点）
                        {
                            continue;
                        }
                        mContourSections[contourSectionIndex].push_back(*vertexVertexIter); //如果以上条件都不满足，则将此边界点添加作为该contour section的1个点
                        mToothMesh.property(mVPropHandleSearchContourSectionVisited, *vertexVertexIter) = true;
                    }
                    if(contourSectionFinished)
                    {
                        break;
                    }
                }

                /*//测试，将每段轮廓曲线段保存到文件
                Mesh::Color colorRed(1.0, 0.0, 0.0);
                stringstream ss;
                ss << contourSectionIndex;
                string contourSectionIndexString = ss.str();
                mExtraMesh = Mesh();
                if(!mExtraMesh.has_vertex_colors())
                {
                    mExtraMesh.request_vertex_colors();
                }
                Mesh::VertexHandle tempVertexHandle;
                for(int contourSectionVertexIndex = 0; contourSectionVertexIndex < mContourSections[contourSectionIndex].size(); contourSectionVertexIndex++)
                {
                    tempVertexHandle = mExtraMesh.add_vertex(mToothMesh.point(mContourSections[contourSectionIndex].at(contourSectionVertexIndex)));
                    mExtraMesh.set_color(tempVertexHandle, colorRed);
                }
                saveExtraMesh(mToothMesh.MeshName.toStdString() + ".RefineToothBoundary.Extra.ContourSection" + contourSectionIndexString + ".off");*/

                contourSectionIndex++;
            }
        }
    }
}

/*void ToothSegmentation::checkCustomMeshPropertiesExistence()
{
    if(!mToothMesh.get_property_handle(mVPropHandleCurvature, mVPropHandleCurvatureName)
            || !mToothMesh.get_property_handle(mVPropHandleCurvatureComputed, mVPropHandleCurvatureComputedName)
            || !mToothMesh.get_property_handle(mVPropHandleIsToothBoundary, mVPropHandleIsToothBoundaryName)
            || !mToothMesh.get_property_handle(mVPropHandleBoundaryVertexType, mVPropHandleBoundaryVertexTypeName)
            || !mToothMesh.get_property_handle(mVPropHandleNonBoundaryRegionType, mVPropHandleNonBoundaryRegionTypeName)
            || !mToothMesh.get_property_handle(mVPropHandleRegionGrowingVisited, mVPropHandleRegionGrowingVisitedName)
            || !mToothMesh.get_property_handle(mVPropHandleBoundaryType, mVPropHandleBoundaryTypeName)
            || !mToothMesh.get_property_handle(mVPropHandleSearchContourSectionVisited, mVPropHandleSearchContourSectionVisitedName))
    {
        throw runtime_error("There should be these properties in mToothMesh:\n"
                            + mVPropHandleCurvatureName + "\n"
                            + mVPropHandleCurvatureComputedName + "\n"
                            + mVPropHandleIsToothBoundaryName + "\n"
                            + mVPropHandleBoundaryVertexTypeName + "\n"
                            + mVPropHandleNonBoundaryRegionTypeName + "\n"
                            + mVPropHandleRegionGrowingVisitedName + "\n"
                            + mVPropHandleBoundaryTypeName + "\n"
                            + mVPropHandleSearchContourSectionVisitedName);
    }
}

float ToothSegmentation::cos(const Mesh::Point &vector1, const Mesh::Point &vector2) const
{
    float a = vector1[0] * vector2[0] + vector1[1] * vector2[1] + vector1[2] * vector2[2];
    float b = (vector1[0] * vector1[0] + vector1[1] * vector1[1] + vector1[2] * vector1[2]) * (vector2[0] * vector2[0] + vector2[1] * vector2[1] + vector2[2] * vector2[2]);
    return a / sqrt(b);
}

float ToothSegmentation::cot(const Mesh::Point &vector1, const Mesh::Point &vector2) const
{
    float a = cos(vector1, vector2);
    return sqrt(1 - a * a) / a;
}*/